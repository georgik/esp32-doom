// ESP32 Doom sound - maps SFX to buzzer beeps
#include "config.h"
#include "i_sound.h"
#include "s_sound.h"
#include "w_wad.h"
#include "lprintf.h"
#include "sndhw.h"
#include "sounds.h"
#include <string.h>
#include <stdio.h>

int snd_card = 1;
int mus_card = 0;
int snd_samplerate = 22050;

extern void doom_play_beep(int type);

#define BEEP_NONE 0
#define BEEP_SHOT 1
#define BEEP_EXPLOSION 2
#define BEEP_PICKUP 3
#define BEEP_DOOR 4
#define BEEP_HURT 5
#define BEEP_DEATH 6
#define BEEP_ENEMY 7
#define BEEP_SWITCH 8

static int sfx_to_beep(int sfx_id) {
    if (sfx_id < 0 || sfx_id >= NUMSFX) return BEEP_NONE;

    const char *name = S_sfx[sfx_id].name;

    // Weapon shots
    if (strcmp(name, "pistol") == 0 ||
        strcmp(name, "shotgn") == 0 ||
        strcmp(name, "plasma") == 0 ||
        strcmp(name, "chgun") == 0) return BEEP_SHOT;

    // Explosions
    if (strcmp(name, "rxplod") == 0 ||
        strcmp(name, "firxpl") == 0 ||
        strcmp(name, "bfg") == 0) return BEEP_EXPLOSION;

    // Pickups
    if (strcmp(name, "itemup") == 0 ||
        strcmp(name, "wpnup") == 0) return BEEP_PICKUP;

    // Doors/platforms
    if (strcmp(name, "doropn") == 0 ||
        strcmp(name, "dorcls") == 0 ||
        strcmp(name, "pstart") == 0 ||
        strcmp(name, "pstop") == 0 ||
        strcmp(name, "stnmov") == 0) return BEEP_DOOR;

    // Player pain
    if (strcmp(name, "plpain") == 0 ||
        strcmp(name, "oof") == 0) return BEEP_HURT;

    // Deaths
    if (strcmp(name, "pldeth") == 0 ||
        strcmp(name, "pdiehi") == 0) return BEEP_DEATH;

    // Enemy attacks
    if (strcmp(name, "claw") == 0 ||
        strcmp(name, "sklatk") == 0 ||
        strcmp(name, "sgtatk") == 0) return BEEP_ENEMY;

    // Switches
    if (strcmp(name, "swtchn") == 0 ||
        strcmp(name, "swtchx") == 0) return BEEP_SWITCH;

    return BEEP_NONE;
}

void I_UpdateSoundParams(int handle, int volume, int seperation, int pitch) {
}

void I_SetChannels(void) {
}

int I_GetSfxLumpNum(sfxinfo_t* sfx) {
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    for (int i = 0; i < 8; i++) {
        if (namebuf[i] >= 'a' && namebuf[i] <= 'z')
            namebuf[i] -= 32;
    }
    return W_CheckNumForName(namebuf);
}

int I_StartSound(int id, int channel, int vol, int sep, int pitch, int priority) {
    if (id >= 0 && id < NUMSFX) {
        int beep_type = sfx_to_beep(id);
        if (beep_type != BEEP_NONE) {
            doom_play_beep(beep_type);
        }
    }
    return channel;
}

void I_StopSound(int handle) {
}

int I_SoundIsPlaying(int handle) {
    return 0;
}

int I_AnySoundStillPlaying(void) {
    return false;
}

void I_ShutdownSound(void) {
    sndhw_deinit();
}

void I_InitSound(void) {
    lprintf(LO_INFO, "I_InitSound: Buzzer sound initialized\n");
}

void I_ShutdownMusic(void) {
}

void I_InitMusic(void) {
}

int I_RegisterSong(const void *data, size_t len) {
    return 0;
}

void I_UnRegisterSong(int handle) {
}

void I_PlaySong(uint8_t *data, int len, int looping) {
}

void I_PauseSong(void) {
}

void I_ResumeSong(void) {
}

void I_StopSong(void) {
}

void I_SetMusicVolume(int volume) {
}
