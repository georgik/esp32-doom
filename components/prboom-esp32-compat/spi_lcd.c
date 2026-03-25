#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "bsp/esp-box-3.h"
#include "bsp/display.h"

static const char *TAG = "doom_lcd";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io_handle;
static uint16_t *s_dma_buf;
static SemaphoreHandle_t s_flush_done;
extern int16_t lcdpal[256];

#define LCD_DMA_LINES 40
#define LCD_ROW_BYTES BSP_LCD_H_RES

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

    for (int y = 0; y < BSP_LCD_V_RES; y += LCD_DMA_LINES) {
        const int lines = MIN(LCD_DMA_LINES, BSP_LCD_V_RES - y);
        const size_t pixels = BSP_LCD_H_RES * lines;
        const uint8_t *span = scr + (y * LCD_ROW_BYTES);

        for (size_t i = 0; i < pixels; i++) {
            s_dma_buf[i] = lcdpal[span[i]];
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_H_RES, y + lines, s_dma_buf));
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
    }
}

void spi_lcd_init(void)
{
    if (s_panel != NULL) {
        return;
    }

    const bsp_display_config_t bsp_disp_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * LCD_DMA_LINES * sizeof(uint16_t),
    };

    s_flush_done = xSemaphoreCreateBinary();
    assert(s_flush_done != NULL);
    ESP_ERROR_CHECK(bsp_display_new(&bsp_disp_cfg, &s_panel, &s_io_handle));
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cbs, NULL));
    s_dma_buf = heap_caps_malloc(BSP_LCD_H_RES * LCD_DMA_LINES * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(s_dma_buf != NULL);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_ERROR_CHECK(bsp_display_brightness_init());
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_LOGI(TAG, "BOX-3 display initialized");
}
