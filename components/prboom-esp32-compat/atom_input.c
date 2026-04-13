/*
 * AtomS3 Joystick Input for Doom
 * Handles I2C joystick and button input for M5Stack AtomS3 + Joystick Unit
 */

#include "atom_input.h"
#include "i2c_joystick.h"
#include "doomdef.h"
#include "doomtype.h"
#include "d_event.h"
#include "d_main.h"
#include "g_game.h"
#include "gamepad.h"
#include "lprintf.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "atom_input";
static i2c_joystick_handle_t s_joystick_handle = {0};
static bool s_initialized = false;

// Joystick state tracking
typedef struct {
    int joy1_x, joy1_y;      // Left joystick (movement)
    int joy2_x, joy2_y;      // Right joystick (turning/alt)
    bool btn_left;           // Face button LEFT
    bool btn_right;          // Face button RIGHT
    bool btn_left_stick;     // Left joystick press
    bool btn_right_stick;    // Right joystick press
    bool btn_builtin;        // GPIO 41 built-in
} atom_joystick_state_t;

static atom_joystick_state_t s_joystick_state = {0};
static atom_joystick_state_t s_prev_joystick_state = {0};

// Joystick thresholds
#define JOY_THRESHOLD_LOW    800
#define JOY_THRESHOLD_HIGH   3200

// Button to Doom key mapping
typedef struct {
    bool *button_state;
    bool *prev_state;
    int *doom_key;
    const char *name;
} button_map_t;

static button_map_t s_button_map[] = {
    {&s_joystick_state.btn_left,        &s_prev_joystick_state.btn_left,        &key_fire,         "Fire"},
    {&s_joystick_state.btn_right,       &s_prev_joystick_state.btn_right,       &key_use,          "Use"},
    {&s_joystick_state.btn_left_stick,  &s_prev_joystick_state.btn_left_stick,  &key_weapontoggle, "Weapon"},
    {&s_joystick_state.btn_right_stick, &s_prev_joystick_state.btn_right_stick, &key_pause,        "Pause"},
    {&s_joystick_state.btn_builtin,     &s_prev_joystick_state.btn_builtin,     &key_escape,       "Escape"},
};

#define NUM_BUTTONS (sizeof(s_button_map) / sizeof(s_button_map[0]))

// Axis state tracking
static struct {
    bool up_active;
    bool down_active;
    bool left_active;
    bool right_active;
    bool joy2_left_active;
    bool joy2_right_active;
} s_axis_state = {0};

// Check if joystick axis is active in a direction
static bool is_axis_active(int value, bool is_up_left) {
    if (is_up_left) {
        return value < JOY_THRESHOLD_LOW;
    } else {
        return value > JOY_THRESHOLD_HIGH;
    }
}

// Map joystick position to Doom key events
static void process_joystick_axis(int value, bool is_up_left, int *doom_key, bool *was_active, const char *name) {
    bool is_active = is_axis_active(value, is_up_left);

    if (is_active && !*was_active) {
        // Axis just activated
        event_t ev = {.type = ev_keydown, .data1 = *doom_key};
        D_PostEvent(&ev);
        ESP_LOGD(TAG, "Joystick %s: DOWN", name);
    } else if (!is_active && *was_active) {
        // Axis just released
        event_t ev = {.type = ev_keyup, .data1 = *doom_key};
        D_PostEvent(&ev);
        ESP_LOGD(TAG, "Joystick %s: UP", name);
    }

    *was_active = is_active;
}

// Process button state changes
static void process_buttons(void) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        bool *current = s_button_map[i].button_state;
        bool *prev = s_button_map[i].prev_state;
        int *doom_key = s_button_map[i].doom_key;
        const char *name = s_button_map[i].name;

        if (*current != *prev) {
            *prev = *current;

            event_t ev = {
                .type = *current ? ev_keydown : ev_keyup,
                .data1 = *doom_key
            };
            D_PostEvent(&ev);

            ESP_LOGD(TAG, "Button %s: %s", name, *current ? "DOWN" : "UP");
        }
    }
}

// Read all joystick data
static void update_joystick_state(void) {
    uint16_t joy1_x, joy1_y;
    bool btn_left, btn_right, btn_left_stick, btn_right_stick;

    // Read Joy1 and face buttons via I2C
    esp_err_t ret = i2c_joystick_read_all(&s_joystick_handle,
                                           &joy1_x, &joy1_y,
                                           &btn_left, &btn_right,
                                           &btn_left_stick, &btn_right_stick);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read joystick data");
        return;
    }

    // Read Joy2 axes separately
    uint16_t joy2_x, joy2_y;
    ret = i2c_joystick_read_axis(&s_joystick_handle, JOY2_X_REG, &joy2_x);
    if (ret != ESP_OK) joy2_x = 2048; // Center on error

    ret = i2c_joystick_read_axis(&s_joystick_handle, JOY2_Y_REG, &joy2_y);
    if (ret != ESP_OK) joy2_y = 2048; // Center on error

    // Update state
    s_joystick_state.joy1_x = joy1_x;
    s_joystick_state.joy1_y = joy1_y;
    s_joystick_state.joy2_x = joy2_x;
    s_joystick_state.joy2_y = joy2_y;
    s_joystick_state.btn_left = btn_left;
    s_joystick_state.btn_right = btn_right;
    s_joystick_state.btn_left_stick = btn_left_stick;
    s_joystick_state.btn_right_stick = btn_right_stick;

    // Read GPIO button (41)
    s_joystick_state.btn_builtin = (gpio_get_level(GPIO_NUM_41) == 0);
}

// Main joystick polling task
static void joystick_task(void *arg) {
    ESP_LOGI(TAG, "AtomS3 joystick task started");

    uint32_t last_log_time = 0;

    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        update_joystick_state();

        // Process Joy1 (movement)
        process_joystick_axis(s_joystick_state.joy1_x, false, &key_right, &s_axis_state.right_active, "RIGHT");
        process_joystick_axis(s_joystick_state.joy1_x, true, &key_left, &s_axis_state.left_active, "LEFT");
        process_joystick_axis(s_joystick_state.joy1_y, true, &key_up, &s_axis_state.up_active, "UP");
        process_joystick_axis(s_joystick_state.joy1_y, false, &key_down, &s_axis_state.down_active, "DOWN");

        // Process Joy2 (currently unused, but available for future mapping)
        // process_joystick_axis(s_joystick_state.joy2_x, false, &key_turnright, &s_axis_state.joy2_right_active, "TURN RIGHT");
        // process_joystick_axis(s_joystick_state.joy2_x, true, &key_turnleft, &s_axis_state.joy2_left_active, "TURN LEFT");

        // Process buttons
        process_buttons();

        // Debug logging (every 2 seconds)
        if (now - last_log_time >= 2000) {
            ESP_LOGI(TAG, "Joy1: X=%d Y=%d | Joy2: X=%d Y=%d | Buttons: L=%d R=%d LS=%d RS=%d",
                     s_joystick_state.joy1_x, s_joystick_state.joy1_y,
                     s_joystick_state.joy2_x, s_joystick_state.joy2_y,
                     s_joystick_state.btn_left, s_joystick_state.btn_right,
                     s_joystick_state.btn_left_stick, s_joystick_state.btn_right_stick);
            last_log_time = now;
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // 50Hz polling
    }
}

// Initialize AtomS3 input system
void atomInputInit(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return;
    }

    // Initialize I2C joystick
    esp_err_t ret = i2c_joystick_init(&s_joystick_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C joystick: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize GPIO button (41)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_41),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    s_initialized = true;
    lprintf(LO_INFO, "AtomS3 input initialized: I2C joystick + GPIO button\n");

    // Create joystick polling task
    xTaskCreatePinnedToCore(joystick_task, "atom_joy", 4096, NULL, 5, NULL, 0);
}

// Get joystick state for gamepad compatibility (PS2 bitmask)
int atomJsInputGet(void) {
    // Return PS2-compatible bitmask for existing code
    int joy_val = 0xFFFF;

    if (s_joystick_state.btn_left)        joy_val &= ~0x4000; // Cross -> Use
    if (s_joystick_state.btn_right)       joy_val &= ~0x2000; // Circle -> Fire
    if (s_joystick_state.btn_left_stick)  joy_val &= ~0x1000; // Triangle -> Weapon
    if (s_joystick_state.btn_right_stick) joy_val &= ~0x8000; // Square -> Pause
    if (s_joystick_state.btn_builtin)     joy_val &= ~0x8;    // Start -> Escape

    // Map joystick axes to D-pad
    if (s_joystick_state.joy1_y < JOY_THRESHOLD_LOW)  joy_val &= ~0x10; // UP
    if (s_joystick_state.joy1_y > JOY_THRESHOLD_HIGH) joy_val &= ~0x40; // DOWN
    if (s_joystick_state.joy1_x < JOY_THRESHOLD_LOW)  joy_val &= ~0x80; // LEFT
    if (s_joystick_state.joy1_x > JOY_THRESHOLD_HIGH) joy_val &= ~0x20; // RIGHT

    return joy_val;
}

