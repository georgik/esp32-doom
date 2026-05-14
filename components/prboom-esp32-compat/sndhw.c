// Buzzer audio for AtomS3R using PWM on GPIO5 (G5)
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "sndhw.h"

static const char *TAG = "buzzer_audio";

#define BUZZER_GPIO     5
#define BUZZER_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZER_LEDC_TIMER   LEDC_TIMER_0
#define BUZZER_LEDC_RES     LEDC_TIMER_10_BIT

static QueueHandle_t beep_queue = NULL;
static volatile bool buzzer_initialized = false;

typedef struct {
    uint32_t freq;
    uint32_t duration_ms;
} beep_event_t;

static void buzzer_init_hw(void) {
    ESP_LOGI(TAG, "Init buzzer on GPIO%d", BUZZER_GPIO);

    ledc_timer_config_t timer_conf = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_LEDC_RES,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_conf);

    // Test beep
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, 1000);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 512);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);

    buzzer_initialized = true;
    ESP_LOGI(TAG, "Buzzer ready (test beep sent)");
}

static void buzzer_beep_now(uint32_t freq, uint32_t duration_ms) {
    if (!buzzer_initialized) return;

    ESP_LOGD(TAG, "Beep: %dHz, %dms", freq, duration_ms);

    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 512);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void beep_task(void *arg) {
    beep_event_t event;
    while (1) {
        if (beep_queue && xQueueReceive(beep_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            buzzer_beep_now(event.freq, event.duration_ms);
        }
    }
}

void sndhw_lock(void) {
}

void sndhw_unlock(void) {
}

void sndhw_init(int samprate, snd_cb_t cb) {
    ESP_LOGI(TAG, "Buzzer audio init");

    beep_queue = xQueueCreate(4, sizeof(beep_event_t));

    buzzer_init_hw();

    xTaskCreate(beep_task, "beep", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Buzzer audio ready");
}

void sndhw_deinit(void) {
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    if (beep_queue) vQueueDelete(beep_queue);
}

void sndhw_start(void) {
}

void sndhw_stop(void) {
}

void doom_play_beep(int type) {
    if (!beep_queue) return;

    beep_event_t event = {0};

    switch (type) {
        case 1: event.freq = 800; event.duration_ms = 50; break;
        case 2: event.freq = 200; event.duration_ms = 150; break;
        case 3: event.freq = 1200; event.duration_ms = 80;
            xQueueSend(beep_queue, &event, 0);
            event.freq = 1600; event.duration_ms = 80;
            break;
        case 4: event.freq = 400; event.duration_ms = 100; break;
        case 5: event.freq = 300; event.duration_ms = 100; break;
        case 6: event.freq = 250; event.duration_ms = 400; break;
        case 7: event.freq = 600; event.duration_ms = 60; break;
        case 8: event.freq = 1000; event.duration_ms = 40; break;
        default: return;
    }

    if (event.freq > 0) {
        xQueueSend(beep_queue, &event, 0);
        ESP_LOGD(TAG, "Beep type %d: %dHz %dms", type, event.freq, event.duration_ms);
    }
}
