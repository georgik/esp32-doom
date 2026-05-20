#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include "bsp/esp-box-3.h"
#include "sndhw.h"

static const char *TAG = "audio";
static SemaphoreHandle_t audio_mux;
static esp_codec_dev_handle_t speaker_dev;
static snd_cb_t audio_cb;
static bool audio_muted = false;

#define SND_CHUNKSZ 560

void sndhw_lock(void)
{
    xSemaphoreTake(audio_mux, portMAX_DELAY);
}

void sndhw_unlock(void)
{
    xSemaphoreGive(audio_mux);
}

static void audio_task(void *arg)
{
    int16_t snd_in[SND_CHUNKSZ] = {0};

    while (1) {
        sndhw_lock();
        audio_cb(snd_in, sizeof(snd_in) / sizeof(snd_in[0]));
        sndhw_unlock();

        if (!audio_muted) {
            int written = esp_codec_dev_write(speaker_dev, snd_in, sizeof(snd_in));
            if (written < 0) {
                ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", written);
                vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                taskYIELD();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void sndhw_init(int samprate, snd_cb_t cb)
{
    if (speaker_dev != NULL) {
        return;
    }

    speaker_dev = bsp_audio_codec_speaker_init();
    assert(speaker_dev);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = samprate,
        .channel = 1,
        .bits_per_sample = 16,
    };

    ESP_ERROR_CHECK(esp_codec_dev_open(speaker_dev, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(speaker_dev, 60));

    audio_mux = xSemaphoreCreateMutex();
    assert(audio_mux);
    audio_cb = cb;

    xTaskCreatePinnedToCore(&audio_task, "snd", 16 * 1024, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "BOX-3 speaker initialized");
}

void sndhw_toggle_mute(void)
{
    audio_muted = !audio_muted;
    ESP_LOGI(TAG, "Audio %s", audio_muted ? "muted" : "unmuted");
}
