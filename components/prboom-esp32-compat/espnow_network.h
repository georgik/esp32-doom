// ESP-NOW network wrapper for Doom multiplayer
#ifndef ESPNOW_NETWORK_H
#define ESPNOW_NETWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define ESPNOW_MAX_PLAYERS 4
#define ESPNOW_ADDR_LEN 6

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr[ESPNOW_ADDR_LEN];
    bool connected;
    int player_num;
} espnow_player_t;

typedef struct {
    espnow_player_t players[ESPNOW_MAX_PLAYERS];
    int num_players;
    uint8_t my_addr[ESPNOW_ADDR_LEN];
    bool is_host;
    bool initialized;
} espnow_state_t;

// Initialize ESP-NOW network
esp_err_t doom_espnow_init(bool as_host);

// Deinitialize ESP-NOW network
esp_err_t doom_espnow_deinit(void);

// Send packet to specific address (broadcast if addr is NULL)
esp_err_t doom_espnow_send(const uint8_t *dest_addr, const void *data, size_t len);

// Receive packet with timeout (-1 = wait forever, 0 = non-blocking)
// Returns bytes received or -1 on timeout/error
int doom_espnow_recv(uint8_t *src_addr, void *data, size_t max_len, int timeout_ms);

// Check if data is available
bool doom_espnow_data_available(void);

// Send to specific player by number
esp_err_t doom_espnow_send_to_player(int player_num, const void *data, size_t len);

// Add/update player in state
esp_err_t doom_espnow_add_player(int player_num, const uint8_t *addr);

// Get player list
const espnow_state_t* doom_espnow_get_state(void);

// Start discovery (as host)
esp_err_t doom_espnow_start_discovery(void);

// Join game (as client)
esp_err_t doom_espnow_join_game(const uint8_t *host_addr);

// Check if initialized
bool doom_espnow_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif // ESPNOW_NETWORK_H
