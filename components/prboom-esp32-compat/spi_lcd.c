#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9a01.h"

static const char *TAG = "doom_lcd";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io_handle;
static uint16_t *s_dma_buf;
static SemaphoreHandle_t s_flush_done;
extern int16_t lcdpal[256];

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
#define LCD_BACKLIGHT     GPIO_NUM_16  // Simple GPIO fallback for now

static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
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

    // Simple GPIO backlight control for now (will implement LP5562 later)
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_conf);
    gpio_set_level(LCD_BACKLIGHT, 1);  // Turn on backlight

    ESP_LOGI(TAG, "AtomS3R display initialized properly (GC9A01 driver, 128x128)");
}
