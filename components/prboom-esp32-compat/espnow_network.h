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

// Peek at packet type without consuming from queue
// Returns packet type or -1 if no packet
int doom_espnow_peek_type(void);

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

// Get ESP-NOW statistics
void doom_espnow_get_stats(int *tx_count, int *rx_count, int *dropped_count);

// Check if TX is busy (previous send not completed)
bool doom_espnow_tx_busy(void);

// Send ticcmd packet to specific player
// ticcmd_t is defined in d_ticcmd.h - include that before this header
esp_err_t doom_espnow_send_tics(int player_num, uint32_t start_tic,
                                const void *tics, int count);

#ifdef __cplusplus
}
#endif

#endif // ESPNOW_NETWORK_H
