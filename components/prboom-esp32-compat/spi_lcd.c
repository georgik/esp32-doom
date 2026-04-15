#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "esp_lcd_gc9a01.h"

static const char *TAG = "doom_lcd";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io_handle;
static uint16_t *s_dma_buf;
static SemaphoreHandle_t s_flush_done;
extern int16_t lcdpal[256];

// Shared I2C bus for LP5562 backlight and potential joystick
static i2c_master_bus_handle_t s_i2c_bus = NULL;

// AtomS3R display configuration (from Rust implementation)
#define LCD_DMA_LINES 32
#define LCD_H_RES 128
#define LCD_V_RES 128
#define LCD_ROW_BYTES LCD_H_RES

// AtomS3R GPIO pins (different from AtomS3)
#define LCD_MOSI          GPIO_NUM_21
#define LCD_SCK           GPIO_NUM_15
#define LCD_CS            GPIO_NUM_14
#define LCD_DC            GPIO_NUM_42
#define LCD_RST           GPIO_NUM_48

// LP5562 backlight driver I2C configuration
#define LP5562_I2C_ADDR           0x30
#define LP5562_I2C_REG_ENABLE     0x00
#define LP5562_I2C_REG_OP_MODE    0x01
#define LP5562_I2C_REG_W_PWM      0x0E
#define LP5562_I2C_REG_W_CURRENT  0x0F
#define LP5562_I2C_REG_CONFIG     0x08
#define LP5562_I2C_REG_LED_MAP    0x70
#define LP5562_I2C_MASTER_ENABLE  0x40

#define I2C_SDA_PIN              GPIO_NUM_45
#define I2C_SCL_PIN              GPIO_NUM_0


static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

// LP5562 backlight driver initialization (matching Rust implementation)
static esp_err_t lp5562_init(void)
{
    esp_err_t ret;
    i2c_master_dev_handle_t lp5562_handle;

    ESP_LOGI(TAG, "Initializing LP5562 backlight driver via I2C (ESP-IDF 6)");

    // Create I2C bus only once
    if (s_i2c_bus == NULL) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .scl_io_num = I2C_SCL_PIN,
            .sda_io_num = I2C_SDA_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };

        ret = i2c_new_master_bus(&bus_config, &s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2C master bus create failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "I2C master bus created for LP5562 (I2C_NUM_0, GPIO %d/%d)",
                 I2C_SCL_PIN, I2C_SDA_PIN);
    } else {
        ESP_LOGI(TAG, "Reusing existing I2C bus for LP5562");
    }

    // Configure LP5562 device
    i2c_device_config_t lp5562_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LP5562_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    ret = i2c_master_bus_add_device(s_i2c_bus, &lp5562_config, &lp5562_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 1: Enable internal clock
    uint8_t config_data[] = {LP5562_I2C_REG_CONFIG, 0x01};
    ret = i2c_master_transmit(lp5562_handle, config_data, sizeof(config_data), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 clock enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    // Step 2: Enable chip
    uint8_t enable_data[] = {LP5562_I2C_REG_ENABLE, LP5562_I2C_MASTER_ENABLE};
    ret = i2c_master_transmit(lp5562_handle, enable_data, sizeof(enable_data), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 chip enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_delay_us(500);

    // Step 3: Configure LED map - all LEDs controlled from I2C registers
    uint8_t ledmap_data[] = {LP5562_I2C_REG_LED_MAP, 0x00};
    ret = i2c_master_transmit(lp5562_handle, ledmap_data, sizeof(ledmap_data), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 LED map config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_delay_us(200);

    // Step 4: Set operation mode to direct PWM control
    uint8_t opmode_data[] = {LP5562_I2C_REG_OP_MODE, 0x00};
    ret = i2c_master_transmit(lp5562_handle, opmode_data, sizeof(opmode_data), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 op mode set failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_delay_us(200);

    // Step 5: Set PWM brightness to maximum (255)
    uint8_t pwm_data[] = {LP5562_I2C_REG_W_PWM, 0xFF};
    ret = i2c_master_transmit(lp5562_handle, pwm_data, sizeof(pwm_data), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 PWM brightness set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Step 6: Set current to maximum
    uint8_t current_data[] = {LP5562_I2C_REG_W_CURRENT, 0xFF};
    ret = i2c_master_transmit(lp5562_handle, current_data, sizeof(current_data), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 current set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "LP5562 backlight initialized at full brightness (ESP-IDF 6 I2C master driver)");
    return ESP_OK;
}

void spi_lcd_wait_finish(void)
{
    if (s_flush_done == NULL) {
        return;
    }

    while (xSemaphoreTake(s_flush_done, 0) == pdTRUE) {
    }
}

void spi_lcd_invalidate(void)
{
}

void spi_lcd_send(const uint8_t *scr)
{
    if (s_panel == NULL) {
        return;
    }

    // Doom renders at 320x240, but AtomS3R display is 128x128
    // Scale and send in chunks to match DMA buffer size
    const int DOOM_WIDTH = 320;
    const int DOOM_HEIGHT = 240;

    // Process display in chunks matching our DMA buffer size
    for (int y = 0; y < LCD_V_RES; y += LCD_DMA_LINES) {
        const int lines = (LCD_DMA_LINES < (LCD_V_RES - y)) ? LCD_DMA_LINES : (LCD_V_RES - y);
        const size_t pixels = LCD_H_RES * lines;

        // Scale this chunk from Doom 320x240 to 128x128
        for (int dy = 0; dy < lines; dy++) {
            int display_y = y + dy;
            for (int x = 0; x < LCD_H_RES; x++) {
                // Calculate source coordinates from Doom screen
                int src_x = (x * DOOM_WIDTH) / LCD_H_RES;
                int src_y = (display_y * DOOM_HEIGHT) / LCD_V_RES;

                // Read pixel from Doom screen buffer (320x240)
                uint8_t doom_pixel = scr[src_y * DOOM_WIDTH + src_x];

                // Convert to RGB565 and store in DMA buffer
                s_dma_buf[dy * LCD_H_RES + x] = lcdpal[doom_pixel];
            }
        }

        // Send this chunk to the display
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + lines, s_dma_buf));
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
    }
}

void spi_lcd_init(void)
{
    if (s_panel != NULL) {
        return;
    }

    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing AtomS3R display with proper configuration");

    // Initialize LP5562 backlight FIRST (required for display visibility)
    ret = lp5562_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LP5562 backlight: %s", esp_err_to_name(ret));
        // Continue anyway - display might still work with previous state
    }

    // Initialize SPI bus with AtomS3R pins (from Conway implementation)
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_DMA_LINES * sizeof(uint16_t),
    };

    ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize LCD panel IO
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize GC9A01 panel with AtomS3R-specific configuration
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };

    ret = esp_lcd_new_panel_gc9a01(s_io_handle, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create GC9A01 panel: %s", esp_err_to_name(ret));
        return;
    }

    s_flush_done = xSemaphoreCreateBinary();
    assert(s_flush_done != NULL);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cbs, NULL));

    s_dma_buf = heap_caps_malloc(LCD_H_RES * LCD_DMA_LINES * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(s_dma_buf != NULL);

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // Configure display for AtomS3R (matching Conway implementation settings)
    // Set display offset and size to match 128x128 AtomS3R screen
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));  // No gap
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));  // Invert colors
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));  // Mirror both axes

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "AtomS3R display initialized properly (GC9A01 driver, 128x128)");
}
