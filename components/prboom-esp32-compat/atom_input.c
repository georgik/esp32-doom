/*
 * AtomS3 Joystick Input for Doom
 * Modern Dual-Stick FPS Controls for M5Stack AtomS3R + Joystick Unit
 *
 * CONTROL MAPPING:
 * Left Joystick (Joy1) - Movement + Speed Modifier:
 *   UP/DOWN    - Move Forward/Backward
 *   LEFT/RIGHT - Strafe Left/Right
 *   PRESS      - Speed (Run) Modifier
 *
 * Right Joystick (Joy2) - Turning + Fire:
 *   LEFT/RIGHT - Turn Left/Right
 *   PRESS      - Fire (Primary Attack)
 *
 * Face Buttons:
 *   LEFT       - Weapon Toggle
 *   RIGHT      - Use (Open Doors/Activate Switches)
 *
 * GPIO Button (GPIO 41):
 *   Built-in button - Escape (Menu)
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
    int joy2_x, joy2_y;      // Right joystick (turning: left/right)
    bool btn_left;           // Face button LEFT (Weapon Toggle)
    bool btn_right;          // Face button RIGHT (Use)
    bool btn_left_stick;     // Left joystick press (Speed modifier)
    bool btn_right_stick;    // Right joystick press (Fire)
    bool btn_builtin;        // GPIO 41 built-in (Escape)
} atom_joystick_state_t;

static atom_joystick_state_t s_joystick_state = {0};
static atom_joystick_state_t s_prev_joystick_state = {0};

// Joystick thresholds
#define JOY_THRESHOLD_LOW    800
#define JOY_THRESHOLD_HIGH   3200

// Button to Doom key mapping (Optimized for dual-stick Doom gameplay)
typedef struct {
    bool *button_state;
    bool *prev_state;
    int *doom_key;
    const char *name;
} button_map_t;

static button_map_t s_button_map[] = {
    // Note: LEFT_STICK and RIGHT_STICK are handled separately as Speed/Fire modifiers
    {&s_joystick_state.btn_right,       &s_prev_joystick_state.btn_right,       &key_use,          "Use"},           // RIGHT button → Use (open doors)
    {&s_joystick_state.btn_left,        &s_prev_joystick_state.btn_left,        &key_weapontoggle, "Weapon"},        // LEFT button → Weapon Toggle
    {&s_joystick_state.btn_builtin,     &s_prev_joystick_state.btn_builtin,     &key_escape,       "Escape"},        // GPIO 41 → Escape (menu)
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

        // Process Joy1 (Left Stick - Movement with Strafing)
        // Modern dual-stick FPS: Left stick = movement (forward/back + strafe left/right)
        process_joystick_axis(s_joystick_state.joy1_x, false, &key_straferight, &s_axis_state.right_active, "STRAFE RIGHT");
        process_joystick_axis(s_joystick_state.joy1_x, true, &key_strafeleft, &s_axis_state.left_active, "STRAFE LEFT");
        process_joystick_axis(s_joystick_state.joy1_y, true, &key_up, &s_axis_state.up_active, "FORWARD");
        process_joystick_axis(s_joystick_state.joy1_y, false, &key_down, &s_axis_state.down_active, "BACKWARD");

        // Process Joy2 (Right Stick - Turning + Fire)
        // Modern dual-stick FPS: Right stick = turning (left/right) + primary action (fire)
        process_joystick_axis(s_joystick_state.joy2_x, true, &key_left, &s_axis_state.joy2_left_active, "TURN LEFT");
        process_joystick_axis(s_joystick_state.joy2_x, false, &key_right, &s_axis_state.joy2_right_active, "TURN RIGHT");

        // Process Joy2 stick press as FIRE (primary action on right stick)
        bool *joy2_fire = &s_joystick_state.btn_right_stick;
        static bool joy2_fire_was_active = false;
        if (*joy2_fire && !joy2_fire_was_active) {
            event_t ev = {.type = ev_keydown, .data1 = key_fire};
            D_PostEvent(&ev);
            ESP_LOGD(TAG, "Joy2 press: Fire DOWN");
        } else if (!*joy2_fire && joy2_fire_was_active) {
            event_t ev = {.type = ev_keyup, .data1 = key_fire};
            D_PostEvent(&ev);
            ESP_LOGD(TAG, "Joy2 press: Fire UP");
        }
        joy2_fire_was_active = *joy2_fire;

        // Process Joy1 stick press as SPEED (run modifier on left stick)
        bool *joy1_speed = &s_joystick_state.btn_left_stick;
        static bool joy1_speed_was_active = false;
        if (*joy1_speed && !joy1_speed_was_active) {
            event_t ev = {.type = ev_keydown, .data1 = key_speed};
            D_PostEvent(&ev);
            ESP_LOGD(TAG, "Joy1 press: Speed DOWN");
        } else if (!*joy1_speed && joy1_speed_was_active) {
            event_t ev = {.type = ev_keyup, .data1 = key_speed};
            D_PostEvent(&ev);
            ESP_LOGD(TAG, "Joy1 press: Speed UP");
        }
        joy1_speed_was_active = *joy1_speed;

        // Process buttons
        process_buttons();

        // Debug logging (every 2 seconds)
        if (now - last_log_time >= 2000) {
            ESP_LOGI(TAG, "Joy1(Move): X=%d Y=%d | Joy2(Turn): X=%d Y=%d | Buttons: Use=%d Wpn=%d Esc=%d",
                     s_joystick_state.joy1_x, s_joystick_state.joy1_y,
                     s_joystick_state.joy2_x, s_joystick_state.joy2_y,
                     s_joystick_state.btn_right, s_joystick_state.btn_left,
                     s_joystick_state.btn_builtin);
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
    lprintf(LO_INFO, "  Joy1: Move + Strafe | Joy2: Turn | Stick Buttons: Speed/Fire\n");
    lprintf(LO_INFO, "  Face Buttons: Weapon/Use | GPIO 41: Escape\n");

    // Create joystick polling task
    xTaskCreatePinnedToCore(joystick_task, "atom_joy", 4096, NULL, 5, NULL, 0);
}

// Get joystick state for gamepad compatibility (PS2 bitmask)
int atomJsInputGet(void) {
    // Return PS2-compatible bitmask for existing code
    int joy_val = 0xFFFF;

    // Modern dual-stick mapping for PS2 compatibility
    if (s_joystick_state.btn_right)       joy_val &= ~0x4000; // RIGHT button -> Use (Cross)
    if (s_joystick_state.btn_left)        joy_val &= ~0x1000; // LEFT button -> Weapon (Triangle)
    if (s_joystick_state.btn_right_stick) joy_val &= ~0x2000; // RIGHT_STICK -> Fire (Circle)
    if (s_joystick_state.btn_left_stick)  joy_val &= ~0x100;  // LEFT_STICK -> Speed (L2)
    if (s_joystick_state.btn_builtin)     joy_val &= ~0x8;    // GPIO 41 -> Escape (Start)

    // Map Joy1 (movement) to D-pad
    if (s_joystick_state.joy1_y < JOY_THRESHOLD_LOW)  joy_val &= ~0x10; // Forward -> UP
    if (s_joystick_state.joy1_y > JOY_THRESHOLD_HIGH) joy_val &= ~0x40; // Backward -> DOWN
    if (s_joystick_state.joy1_x < JOY_THRESHOLD_LOW)  joy_val &= ~0x80; // Strafe Left -> LEFT
    if (s_joystick_state.joy1_x > JOY_THRESHOLD_HIGH) joy_val &= ~0x20; // Strafe Right -> RIGHT

    // Map Joy2 (turning) to L1/R1 strafe buttons (repurposed for turning)
    if (s_joystick_state.joy2_x < JOY_THRESHOLD_LOW)  joy_val &= ~0x400; // Turn Left -> L1
    if (s_joystick_state.joy2_x > JOY_THRESHOLD_HIGH) joy_val &= ~0x800; // Turn Right -> R1

    return joy_val;
}

