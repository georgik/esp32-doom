#include <stdint.h>
#include "tsinput.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-box-3.h"
#include "bsp/touch.h"
#include "iot_button.h"
#include "sndhw.h"

#include "driver/gpio.h"

#define HC165_PL    38
#define HC165_CE    39
#define HC165_SCL   40
#define HC165_DATA  41

// Shift register bit positions (active low - 0 = pressed)
#define HC165_BIT_UP        12
#define HC165_BIT_DOWN      13
#define HC165_BIT_LEFT      14
#define HC165_BIT_RIGHT     15
#define HC165_BIT_A         0
#define HC165_BIT_B         1
#define HC165_BIT_X         2
#define HC165_BIT_Y         3
#define HC165_BIT_START     9
#define HC165_BIT_SELECT    8
#define HC165_BIT_R1        5   // RB in esp-box terminology = R1
#define HC165_BIT_L1        4   // LB in esp-box terminology = L1

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
#define HC165_MASK_R1       (1U << HC165_BIT_R1)
#define HC165_MASK_L1       (1U << HC165_BIT_L1)

// Shift register to Doom bitmask conversion (both active-low).
// Bit = 1 means NOT pressed, bit = 0 means pressed.
// Unmapped positions stay = 1 so touch input is preserved via AND combing.
static inline uint32_t hc165_to_doom(uint16_t raw)
{
    // Start with all bits set (all buttons not pressed / unmapped)
    uint32_t d = ~0U;

    // D-pad: bits 12-15 -> Doom bits 0-3
    if (!(raw & HC165_MASK_UP))       d &= ~BUT_UP;
    if (!(raw & HC165_MASK_DOWN))     d &= ~BUT_DOWN;
    if (!(raw & HC165_MASK_LEFT))     d &= ~BUT_LEFT;
    if (!(raw & HC165_MASK_RIGHT))    d &= ~BUT_RIGHT;

    // Action buttons mapped to Doom bitmask positions
    if (!(raw & HC165_MASK_A))        d &= ~BUT_CIRCLE;
    if (!(raw & HC165_MASK_B))        d &= ~BUT_CROSS;
    if (!(raw & HC165_MASK_X))        d &= ~BUT_SQUARE;
    if (!(raw & HC165_MASK_Y))        d &= ~BUT_TRIANGLE;

    // Start/Select
    if (!(raw & HC165_MASK_START))   d &= ~BUT_START;
    if (!(raw & HC165_MASK_SELECT))  d &= ~BUT_SELECT;

    // L/R shoulder buttons
    if (!(raw & HC165_MASK_L1))      d &= ~BUT_L1;
    if (!(raw & HC165_MASK_R1))      d &= ~BUT_R1;

    return d;
}

static esp_lcd_touch_handle_t tp;
static bool hc165_initialized = false;
static button_handle_t bsp_buttons[BSP_BUTTON_NUM];

static void mute_button_cb(void *arg, void *data)
{
    sndhw_toggle_mute();
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

    return btn;
}
