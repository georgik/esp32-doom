/*
 * AtomS3 Joystick Input for Doom
 * Modern Dual-Stick FPS Controls for M5Stack AtomS3R + Joystick Unit
 *
 * CONTROL MAPPING:
 * Left Joystick (Joy1) - Movement:
 *   UP/DOWN    - Move Forward/Backward
 *   LEFT/RIGHT - Strafe Left/Right
 *   PRESS      - Fire (Primary Attack)
 *
 * Right Joystick (Joy2) - Movement + Strafe:
 *   UP/DOWN    - Move Forward/Backward (same as left stick)
 *   LEFT/RIGHT - Strafe Left/Right
 *   PRESS      - Fire (Primary Attack)
 *
 * Face Buttons:
 *   LEFT       - Escape (Menu)
 *   RIGHT      - Weapon Change
 *
 * GPIO Button (GPIO 41):
 *   Built-in button - Use (Open Doors/Activate Switches)
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
    int joy1_x, joy1_y;      // Left joystick (movement: forward/back + strafe)
    int joy2_x, joy2_y;      // Right joystick (movement: forward/back + strafe)
    bool btn_left;           // Face button LEFT (Escape)
    bool btn_right;          // Face button RIGHT (Weapon Change)
    bool btn_left_stick;     // Left joystick press (Fire)
    bool btn_right_stick;    // Right joystick press (Fire)
    bool btn_builtin;        // GPIO 41 built-in (Use)
    bool btn_left_last;      // Previous value of LEFT button for debouncing
    bool btn_right_last;     // Previous value of RIGHT button for debouncing
    bool btn_builtin_last;   // Previous value of builtin button for debouncing
} atom_joystick_state_t;

static atom_joystick_state_t s_joystick_state = {0};
static atom_joystick_state_t s_prev_joystick_state = {0};

// Joystick thresholds
#define JOY_THRESHOLD_LOW    800
#define JOY_THRESHOLD_HIGH   3200

// Button to Doom key mapping (Optimized for dual-stick Doom gameplay)
typedef struct {
    bool *button_state;     // Current button state
    bool *prev_state;       // Previously registered state (for Doom events)
    bool *last_state;       // Last read state (for debouncing)
    int *doom_key;
    const char *name;
    int debounce_count;     // Debounce counter (number of stable polls required)
    int current_count;      // Current debounce count
} button_map_t;

static button_map_t s_button_map[] = {
    // Note: LEFT_STICK and RIGHT_STICK are handled separately as Fire
    // debounce_count: number of stable polls (at 50Hz) before registering button press
    {
        &s_joystick_state.btn_left,        // button_state
        &s_prev_joystick_state.btn_left,   // prev_state
        &s_joystick_state.btn_left_last,   // last_state
        &key_escape,                       // doom_key
        "Escape",                          // name
        5,                                 // debounce_count
        0                                  // current_count
    },
    {
        &s_joystick_state.btn_right,       // button_state
        &s_prev_joystick_state.btn_right,  // prev_state
        &s_joystick_state.btn_right_last,  // last_state
        &key_weapontoggle,                 // doom_key
        "Weapon",                          // name
        3,                                 // debounce_count
        0                                  // current_count
    },
    {
        &s_joystick_state.btn_builtin,     // button_state
        &s_prev_joystick_state.btn_builtin,// prev_state
        &s_joystick_state.btn_builtin_last,// last_state
        &key_use,                          // doom_key
        "Use",                             // name
        2,                                 // debounce_count
        0                                  // current_count
    },
};

#define NUM_BUTTONS (sizeof(s_button_map) / sizeof(s_button_map[0]))

// Axis state tracking
static struct {
    bool joy1_up_active;
    bool joy1_down_active;
    bool joy1_left_active;
    bool joy1_right_active;
    bool joy2_up_active;
    bool joy2_down_active;
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

// Process button state changes with debouncing
static void process_buttons(void) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        bool *current = s_button_map[i].button_state;
        bool *prev = s_button_map[i].prev_state;
        bool *last = s_button_map[i].last_state;
        int *doom_key = s_button_map[i].doom_key;
        const char *name = s_button_map[i].name;
        int *debounce_count = &s_button_map[i].debounce_count;
        int *current_count = &s_button_map[i].current_count;

        // Check if button state changed from last read (detect bouncing)
        if (*current != *last) {
            // State changed, reset debounce counter
            *current_count = 0;
        } else {
            // State is stable, increment counter
            if (*current_count < *debounce_count) {
                (*current_count)++;
            }
        }

        // Only register event after debounce period and when state differs from previously registered
        if (*current_count == *debounce_count && *current != *prev) {
            // Button has been stable for debounce period, register the event
            *prev = *current;

            event_t ev = {
                .type = *current ? ev_keydown : ev_keyup,
                .data1 = *doom_key
            };
            D_PostEvent(&ev);

            ESP_LOGD(TAG, "Button %s: %s (debounced %d polls)", name, *current ? "DOWN" : "UP", *debounce_count);
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

    // Update state (save previous button values for debouncing)
    s_joystick_state.joy1_x = joy1_x;
    s_joystick_state.joy1_y = joy1_y;
    s_joystick_state.joy2_x = joy2_x;
    s_joystick_state.joy2_y = joy2_y;
    s_joystick_state.btn_left_last = s_joystick_state.btn_left;
    s_joystick_state.btn_right_last = s_joystick_state.btn_right;
    s_joystick_state.btn_builtin_last = s_joystick_state.btn_builtin;
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

        // Process Joy1 (Left Stick) - Movement + Strafing
        process_joystick_axis(s_joystick_state.joy1_x, false, &key_straferight, &s_axis_state.joy1_right_active, "JOY1 STRAFE RIGHT");
        process_joystick_axis(s_joystick_state.joy1_x, true, &key_strafeleft, &s_axis_state.joy1_left_active, "JOY1 STRAFE LEFT");
        process_joystick_axis(s_joystick_state.joy1_y, true, &key_up, &s_axis_state.joy1_up_active, "JOY1 FORWARD");
        process_joystick_axis(s_joystick_state.joy1_y, false, &key_down, &s_axis_state.joy1_down_active, "JOY1 BACKWARD");

        // Process Joy2 (Right Stick) - Movement + Strafing (same as left stick)
        process_joystick_axis(s_joystick_state.joy2_x, false, &key_straferight, &s_axis_state.joy2_right_active, "JOY2 STRAFE RIGHT");
        process_joystick_axis(s_joystick_state.joy2_x, true, &key_strafeleft, &s_axis_state.joy2_left_active, "JOY2 STRAFE LEFT");
        process_joystick_axis(s_joystick_state.joy2_y, true, &key_up, &s_axis_state.joy2_up_active, "JOY2 FORWARD");
        process_joystick_axis(s_joystick_state.joy2_y, false, &key_down, &s_axis_state.joy2_down_active, "JOY2 BACKWARD");

        // Process both joystick presses as FIRE
        bool *joy1_fire = &s_joystick_state.btn_left_stick;
        bool *joy2_fire = &s_joystick_state.btn_right_stick;
        static bool joy1_fire_was_active = false;
        static bool joy2_fire_was_active = false;
        bool combined_fire = *joy1_fire || *joy2_fire;
        static bool combined_fire_was_active = false;

        if (combined_fire && !combined_fire_was_active) {
            event_t ev = {.type = ev_keydown, .data1 = key_fire};
            D_PostEvent(&ev);
            ESP_LOGD(TAG, "Joystick press: Fire DOWN");
        } else if (!combined_fire && combined_fire_was_active) {
            event_t ev = {.type = ev_keyup, .data1 = key_fire};
            D_PostEvent(&ev);
            ESP_LOGD(TAG, "Joystick press: Fire UP");
        }
        joy1_fire_was_active = *joy1_fire;
        joy2_fire_was_active = *joy2_fire;
        combined_fire_was_active = combined_fire;

        // Process buttons
        process_buttons();

        // Debug logging (every 2 seconds)
        if (now - last_log_time >= 2000) {
            ESP_LOGI(TAG, "Joy1: X=%d Y=%d | Joy2: X=%d Y=%d | Buttons: Esc=%d Wpn=%d Use=%d Fire=%d",
                     s_joystick_state.joy1_x, s_joystick_state.joy1_y,
                     s_joystick_state.joy2_x, s_joystick_state.joy2_y,
                     s_joystick_state.btn_left, s_joystick_state.btn_right,
                     s_joystick_state.btn_builtin, combined_fire);
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
    lprintf(LO_INFO, "AtomS3R dual-stick controls initialized:\n");
    lprintf(LO_INFO, "  Joy1: Move + Strafe | Joy2: Move + Strafe | Both Sticks Press: Fire\n");
    lprintf(LO_INFO, "  Face Buttons: Escape (LEFT) / Weapon Change (RIGHT)\n");
    lprintf(LO_INFO, "  GPIO 41: Use (Open Doors)\n");

    // Create joystick polling task
    xTaskCreatePinnedToCore(joystick_task, "atom_joy", 4096, NULL, 5, NULL, 0);
}

// Get joystick state for gamepad compatibility (PS2 bitmask)
int atomJsInputGet(void) {
    // Return PS2-compatible bitmask for existing code
    int joy_val = 0xFFFF;

    // Updated dual-stick mapping for PS2 compatibility
    if (s_joystick_state.btn_left)        joy_val &= ~0x8;    // LEFT button -> Escape (Start)
    if (s_joystick_state.btn_right)       joy_val &= ~0x1000; // RIGHT button -> Weapon (Triangle)
    if (s_joystick_state.btn_left_stick || s_joystick_state.btn_right_stick)
                                         joy_val &= ~0x2000; // Either stick press -> Fire (Circle)
    if (s_joystick_state.btn_builtin)     joy_val &= ~0x4000; // GPIO 41 -> Use (Cross)

    // Map Joy1 (movement) to D-pad
    if (s_joystick_state.joy1_y < JOY_THRESHOLD_LOW)  joy_val &= ~0x10; // Forward -> UP
    if (s_joystick_state.joy1_y > JOY_THRESHOLD_HIGH) joy_val &= ~0x40; // Backward -> DOWN
    if (s_joystick_state.joy1_x < JOY_THRESHOLD_LOW)  joy_val &= ~0x80; // Strafe Left -> LEFT
    if (s_joystick_state.joy1_x > JOY_THRESHOLD_HIGH) joy_val &= ~0x20; // Strafe Right -> RIGHT

    // Map Joy2 (movement + strafe) to L1/R1 (also strafe)
    if (s_joystick_state.joy2_x < JOY_THRESHOLD_LOW)  joy_val &= ~0x400; // Strafe Left -> L1
    if (s_joystick_state.joy2_x > JOY_THRESHOLD_HIGH) joy_val &= ~0x800; // Strafe Right -> R1

    return joy_val;
}

