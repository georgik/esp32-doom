#include <stdint.h>
#include "tsinput.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-box-3.h"
#include "bsp/touch.h"
#include "iot_button.h"
#include "sndhw.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "driver/gpio.h"

#define HC165_PL    38
#define HC165_CE    39
#define HC165_SCL   40
#define HC165_DATA  41

// ADC channels for joysticks
#define ADC_JOY_LEFT_X    ADC_CHANNEL_0  // Left joystick X-axis
#define ADC_JOY_LEFT_Y    ADC_CHANNEL_1  // Left joystick Y-axis
#define ADC_JOY_RIGHT_X   ADC_CHANNEL_2  // Right joystick X-axis
#define ADC_JOY_RIGHT_Y   ADC_CHANNEL_3  // Right joystick Y-axis
#define ADC_ATTEN         ADC_ATTEN_DB_12
#define ADC_UNIT          ADC_UNIT_2

// Shift register bit positions (active low - 0 = pressed)
// Matches esp-box joystick controller mapping
#define HC165_BIT_UP        0
#define HC165_BIT_LEFT      1
#define HC165_BIT_DOWN      2
#define HC165_BIT_RIGHT     3
#define HC165_BIT_LB        4   // LB button
#define HC165_BIT_LT        5   // LT button
#define HC165_BIT_SELECT    6   // Select button
#define HC165_BIT_L_ROCKER  7   // Left rocker (special, active high)
#define HC165_BIT_Y         8   // Y button
#define HC165_BIT_X         9   // X button
#define HC165_BIT_A         10  // A button
#define HC165_BIT_B         11  // B button
#define HC165_BIT_RB        12  // RB button
#define HC165_BIT_RT        13  // RT button
#define HC165_BIT_START     14  // Start button
#define HC165_BIT_R_ROCKER  15  // Right rocker (special, active high)

// Button bit positions in shift register word
#define HC165_MASK_UP       (1U << HC165_BIT_UP)
#define HC165_MASK_DOWN     (1U << HC165_BIT_DOWN)
#define HC165_MASK_LEFT     (1U << HC165_BIT_LEFT)
#define HC165_MASK_RIGHT    (1U << HC165_BIT_RIGHT)
#define HC165_MASK_A        (1U << HC165_BIT_A)
#define HC165_MASK_B        (1U << HC165_BIT_B)
#define HC165_MASK_X        (1U << HC165_BIT_X)
#define HC165_MASK_Y        (1U << HC165_BIT_Y)
#define HC165_MASK_START    (1U << HC165_BIT_START)
#define HC165_MASK_SELECT   (1U << HC165_BIT_SELECT)
#define HC165_MASK_RB       (1U << HC165_BIT_RB)
#define HC165_MASK_LB       (1U << HC165_BIT_LB)

// Shift register to Doom bitmask conversion (both active-low).
// Bit = 1 means NOT pressed, bit = 0 means pressed.
// Unmapped positions stay = 1 so touch input is preserved via AND combing.
static inline uint32_t hc165_to_doom(uint16_t raw)
{
    // Start with all bits set (all buttons not pressed / unmapped)
    uint32_t d = ~0U;

    // D-pad: bits 0-3 -> Doom bits 0-3
    if (!(raw & HC165_MASK_UP))       d &= ~BUT_UP;
    if (!(raw & HC165_MASK_DOWN))     d &= ~BUT_DOWN;
    if (!(raw & HC165_MASK_LEFT))     d &= ~BUT_LEFT;
    if (!(raw & HC165_MASK_RIGHT))    d &= ~BUT_RIGHT;

    // Action buttons mapped to Doom bitmask positions
    if (!(raw & HC165_MASK_A))        d &= ~BUT_CROSS;      // A = Use
    if (!(raw & HC165_MASK_B))        d &= ~BUT_CIRCLE;     // B = Fire
    if (!(raw & HC165_MASK_X))        d &= ~BUT_TRIANGLE;   // X = Weapon toggle
    if (!(raw & HC165_MASK_Y))        d &= ~BUT_SQUARE;     // Y = Pause/Menu

    // Start/Select
    if (!(raw & HC165_MASK_START))    d &= ~BUT_START;
    if (!(raw & HC165_MASK_SELECT))   d &= ~BUT_SELECT;

    // L/R shoulder buttons (RB/LB)
    if (!(raw & HC165_MASK_LB))       d &= ~BUT_L1;
    if (!(raw & HC165_MASK_RB))       d &= ~BUT_R1;

    return d;
}

static esp_lcd_touch_handle_t tp;
static bool hc165_initialized = false;
static button_handle_t bsp_buttons[BSP_BUTTON_NUM];

// ADC handles for joysticks
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cal_left_x = NULL;
static adc_cali_handle_t adc_cal_left_y = NULL;
static bool adc_initialized = false;

// Deadzone for joystick (center position tolerance)
#define JOY_DEADZONE  500
#define JOY_THRESHOLD  1500  // Threshold to consider joystick moved

static void mute_button_cb(void *arg, void *data)
{
    sndhw_toggle_mute();
}

static void adc_init(void)
{
    if (adc_initialized) return;

    // Init ADC unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    // Config channels
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN,
    };
    adc_oneshot_config_channel(adc_handle, ADC_JOY_LEFT_X, &config);
    adc_oneshot_config_channel(adc_handle, ADC_JOY_LEFT_Y, &config);

    // Try calibration (optional, OK if fails)
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cal_left_x);
    adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cal_left_y);

    adc_initialized = true;
}

// Read joystick and convert to Doom bitmask additions
// Returns bitmask with direction bits set based on joystick position
static uint32_t joystick_to_doom(void)
{
    if (!adc_initialized) return 0;

    int raw_x = 0, raw_y = 0;
    uint32_t result = 0;

    // Read left joystick
    adc_oneshot_read(adc_handle, ADC_JOY_LEFT_X, &raw_x);
    adc_oneshot_read(adc_handle, ADC_JOY_LEFT_Y, &raw_y);

    // Convert calibrated if available, otherwise use raw
    int x = raw_x, y = raw_y;
    if (adc_cal_left_x) {
        adc_cali_raw_to_voltage(adc_cal_left_x, raw_x, &x);
    }
    if (adc_cal_left_y) {
        adc_cali_raw_to_voltage(adc_cal_left_y, raw_y, &y);
    }

    // Center is around 1500-1700mV (or 2048 raw)
    // Left joystick X: left = turn left, right = turn right
    // Left joystick Y: up = forward, down = backward
    int center = 1500;

    if (x < center - JOY_THRESHOLD) {
        result |= BUT_LEFT;   // Turn left
    } else if (x > center + JOY_THRESHOLD) {
        result |= BUT_RIGHT;  // Turn right
    }

    if (y < center - JOY_THRESHOLD) {
        result |= BUT_UP;     // Forward
    } else if (y > center + JOY_THRESHOLD) {
        result |= BUT_DOWN;   // Backward
    }

    return result;
}

static void hc165_init(void)
{
    if (hc165_initialized) return;

    gpio_reset_pin(HC165_PL);
    gpio_reset_pin(HC165_CE);
    gpio_reset_pin(HC165_SCL);
    gpio_reset_pin(HC165_DATA);

    gpio_set_direction(HC165_PL, GPIO_MODE_OUTPUT);
    gpio_set_direction(HC165_CE, GPIO_MODE_OUTPUT);
    gpio_set_direction(HC165_SCL, GPIO_MODE_OUTPUT);
    gpio_set_direction(HC165_DATA, GPIO_MODE_INPUT);

    gpio_set_level(HC165_PL, 1);
    gpio_set_level(HC165_CE, 0);
    gpio_set_level(HC165_SCL, 1);

    hc165_initialized = true;
}

static uint16_t hc165_read(void)
{
    uint16_t data = 0;

    if (!hc165_initialized) return 0;

    gpio_set_level(HC165_PL, 0);
    gpio_set_level(HC165_PL, 1);

    data = (uint16_t)gpio_get_level(HC165_DATA);

    for (int i = 0; i < 15; i++) {
        data <<= 1;

        gpio_set_level(HC165_SCL, 0);
        gpio_set_level(HC165_SCL, 1);

        data |= (uint16_t)gpio_get_level(HC165_DATA);
    }

    return data;
}

void tsJsInputInit(void)
{
    hc165_init();
    adc_init();

    // Initialize BSP buttons (including mute)
    int btn_cnt = 0;
    if (bsp_iot_button_create(bsp_buttons, &btn_cnt, BSP_BUTTON_NUM) == ESP_OK) {
        if (btn_cnt > BSP_BUTTON_MUTE && bsp_buttons[BSP_BUTTON_MUTE] != NULL) {
            iot_button_register_cb(bsp_buttons[BSP_BUTTON_MUTE], BUTTON_PRESS_DOWN, NULL, mute_button_cb, NULL);
        }
    }

    // Touch is optional fallback - don't fail if it doesn't init
    // bsp_touch_new may conflict with already-initialized display
    if (tp == NULL) {
        bsp_touch_new(NULL, &tp);
    }
}

typedef struct {
    int x;
    int y;
    int b;
} v_but_t;

#define BUT_R 32

static const v_but_t v_but[] = {
    // D-pad (circle pattern, center at 64,120)
    {64, 120 - 64, BUT_UP},       // up
    {64, 120 + 64, BUT_DOWN},     // down
    {32, 120, BUT_LEFT},          // left
    {64 + 32, 120, BUT_RIGHT},    // right
    
    // Action buttons (diamond pattern on right side)
    {320 - 32, 120 + 64, BUT_CIRCLE},   // fire
    {320 - 32, 120, BUT_CROSS},         // use
    {320 - 32, 120 - 64, BUT_SQUARE},   // pause/menu
    {320 - 32, 120 - 128, BUT_TRIANGLE},// weapon toggle
    
    // Start/Select (centered below D-pad area)
    {195, 80, BUT_START},
    {125, 80, BUT_SELECT},
    
    // Shoulder buttons (top of screen, left and right sides)
    {60, 40, BUT_L1},           // L1 - strafe left
    {260, 40, BUT_R1},          // R1 - strafe right
    
    {0, 0, 0}  // terminator
};

int tsJsInputGet(void)
{
    esp_lcd_touch_point_data_t touch[8];
    uint8_t touch_cnt = 0;
    int btn = 0xffff;

    if (tp != NULL) {
        esp_lcd_touch_read_data(tp);
        if (esp_lcd_touch_get_data(tp, touch, &touch_cnt, 8) == ESP_OK) {
            for (int i = 0; i < touch_cnt; i++) {
                if (touch[i].strength == 0) {
                    continue;
                }

                for (int j = 0; v_but[j].b != 0; j++) {
                    if (touch[i].x > v_but[j].x - BUT_R && touch[i].x < v_but[j].x + BUT_R &&
                        touch[i].y > v_but[j].y - BUT_R && touch[i].y < v_but[j].y + BUT_R) {
                        btn &= ~v_but[j].b;
                    }
                }
            }
        }
    }

    uint16_t raw = hc165_read();
    btn &= hc165_to_doom(raw);

    // Joystick overrides D-pad for movement (but keeps action buttons)
    uint32_t joy = joystick_to_doom();
    if (joy & (BUT_UP | BUT_DOWN | BUT_LEFT | BUT_RIGHT)) {
        // Clear movement bits, then set joystick movement
        btn &= ~(BUT_UP | BUT_DOWN | BUT_LEFT | BUT_RIGHT);
        btn |= joy;
    }

    return btn;
}
