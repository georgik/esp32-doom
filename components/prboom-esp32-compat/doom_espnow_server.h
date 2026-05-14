// Doom ESP-NOW Server Protocol
#ifndef DOOM_ESPNOW_SERVER_H
#define DOOM_ESPNOW_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Server configuration
typedef struct {
    uint8_t skill;
    uint8_t episode;
    uint8_t level;
    uint8_t deathmatch;
    uint8_t num_players;
} doom_server_config_t;

// Initialize Doom server (call on host device)
void doom_server_init(const doom_server_config_t *config);

// Check if server has a client connected
bool doom_server_has_client(void);

// Get player number (0 for host, 1 for client)
int doom_server_get_player_num(void);

// Server tick - call from main game loop to handle network
void doom_server_tick(void);

// Client functions
bool doom_client_connected(void);
int doom_client_get_player_num(void);

// Client init - send init packet to server
void doom_client_send_init(void);

#ifdef __cplusplus
}
#endif

#endif // DOOM_ESPNOW_SERVER_H
