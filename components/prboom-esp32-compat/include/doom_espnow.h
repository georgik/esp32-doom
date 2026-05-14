// Doom ESP-NOW Auto-Discovery Header
#ifndef DOOM_ESPNOW_H
#define DOOM_ESPNOW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Run auto-discovery and setup
// Returns: 0 if host, 1 if client, -1 on error
int doom_espnow_setup(void);

// Check if this device is host
bool doom_espnow_is_host(void);

// Get player number (0 or 1)
int doom_espnow_get_player_num(void);

// Check if multiplayer (2 players connected)
bool doom_espnow_is_multiplayer(void);

#ifdef __cplusplus
}
#endif

#endif // DOOM_ESPNOW_H
