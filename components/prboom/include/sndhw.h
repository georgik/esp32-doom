#include <stdint.h>

typedef void (*snd_cb_t)(int16_t *buf, int len);

void sndhw_lock();
void sndhw_unlock();
void sndhw_init(int rate, snd_cb_t cb);
void sndhw_deinit();
void sndhw_start();
void sndhw_stop();

// Buzzer helper for AtomS3R
void doom_play_beep(int type);

