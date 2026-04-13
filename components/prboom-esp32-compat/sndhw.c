// Stub audio for AtomS3 (no audio hardware)
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "sndhw.h"

static const char *TAG = "audio_stub";

void sndhw_lock(void) {
}

void sndhw_unlock(void) {
}

void sndhw_init(int samprate, snd_cb_t cb) {
    ESP_LOGW(TAG, "Audio not supported on AtomS3");
}

void sndhw_deinit(void) {
}

void sndhw_start(void) {
}

void sndhw_stop(void) {
}
