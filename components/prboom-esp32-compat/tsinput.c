#include <stdint.h>
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-box-3.h"
#include "bsp/touch.h"

static esp_lcd_touch_handle_t tp;

void tsJsInputInit(void)
{
    if (tp != NULL) {
        return;
    }

    ESP_ERROR_CHECK(bsp_touch_new(NULL, &tp));
}

typedef struct {
    int x;
    int y;
    int b;
} v_but_t;

#define BUT_R 32

static const v_but_t v_but[] = {
    {64, 120 - 64, 0x10},
    {64, 120 + 64, 0x40},
    {32, 120, 0x80},
    {32 + 64, 120, 0x20},
    {320 - 32, 120 + 64, 0x1000},
    {320 - 32, 120, 0x2000},
    {320 - 32, 120 - 64, 0x4000},
    {320 - 32, 120 - 128, 0x8},
    {0, 0, 0}
};

int tsJsInputGet(void)
{
    esp_lcd_touch_point_data_t touch[8];
    uint8_t touch_cnt = 0;
    int btn = 0xffff;

    if (tp == NULL) {
        return btn;
    }

    esp_lcd_touch_read_data(tp);
    if (esp_lcd_touch_get_data(tp, touch, &touch_cnt, 8) != ESP_OK) {
        return btn;
    }

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

    return btn;
}
