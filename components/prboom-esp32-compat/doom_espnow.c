// Doom ESP-NOW Auto-Discovery and Multiplayer Setup
#include "doom_espnow.h"
#include "espnow_network.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "doom_espnow";

#define DISCOVERY_TIMEOUT_MS 3000
#define DISCOVERY_INTERVAL_MS 500
#define HOST_WAITING_TIMEOUT_MS 30000  // Wait 30s for client, then start single-player
#define JOIN_TIMEOUT_MS 10000
#define GAME_START_DELAY_MS 1000

typedef enum {
    STATE_INIT,
    STATE_DISCOVERING,
    STATE_HOST_WAITING,
    STATE_CLIENT_JOINING,
    STATE_READY,
    STATE_ERROR
} doom_espnow_state_t;

typedef struct {
    uint8_t my_mac[6];
    uint8_t host_mac[6];
    int my_player_num;
    bool is_host;
    doom_espnow_state_t state;
    int players_found;
    TickType_t state_enter_time;
} doom_espnow_t;

static doom_espnow_t g_doom_espnow = {0};

// Discovery packet structure
typedef struct {
    char magic[8];        // "DOOM_DISC"
    uint8_t mac[6];       // Sender MAC
    uint8_t player_num;   // 0 for host, 255 for unknown
    uint8_t num_players;  // Current player count
    uint8_t state;        // Current state
} __attribute__((packed)) discovery_packet_t;

// Join packet structure
typedef struct {
    char magic[8];        // "DOOM_JOIN"
    uint8_t mac[6];       // Client MAC
} __attribute__((packed)) join_packet_t;

// Ready packet structure
typedef struct {
    char magic[8];        // "DOOM_READY"
    uint8_t mac[6];       // Sender MAC
    uint8_t player_num;   // Player number
} __attribute__((packed)) ready_packet_t;

// Process discovery packet from other device
static void process_discovery(const uint8_t *src_addr, const uint8_t *data, int len) {
    ESP_LOGI(TAG, "RX discovery from %02x:%02x:%02x:%02x:%02x:%02x, len=%d",
             src_addr[0], src_addr[1], src_addr[2], src_addr[3], src_addr[4], src_addr[5], len);

    if (len < sizeof(discovery_packet_t)) {
        ESP_LOGW(TAG, "Discovery packet too short: %d < %d", len, sizeof(discovery_packet_t));
        return;
    }

    discovery_packet_t *pkt = (discovery_packet_t*)data;

    if (memcmp(pkt->magic, "DOOM_DISC", 8) != 0) {
        ESP_LOGW(TAG, "Invalid discovery magic");
        return;
    }

    ESP_LOGI(TAG, "Discovery: player=%d, num_players=%d, state=%d",
             pkt->player_num, pkt->num_players, pkt->state);

    // Ignore own broadcasts
    if (memcmp(pkt->mac, g_doom_espnow.my_mac, 6) == 0) {
        ESP_LOGD(TAG, "Ignoring own broadcast");
        return;
    }

    if (g_doom_espnow.state == STATE_DISCOVERING) {
        // Found a host!
        if (pkt->player_num == 0) {
            ESP_LOGI(TAG, "*** FOUND HOST! Becoming client... ***");
            memcpy(g_doom_espnow.host_mac, src_addr, 6);
            g_doom_espnow.is_host = false;
            g_doom_espnow.my_player_num = 1;  // Client is player 1
            g_doom_espnow.state = STATE_CLIENT_JOINING;
            g_doom_espnow.state_enter_time = xTaskGetTickCount();

            // Send join request immediately
            join_packet_t join;
            memcpy(join.magic, "DOOM_JOIN", 8);
            memcpy(join.mac, g_doom_espnow.my_mac, 6);
            doom_espnow_send(src_addr, &join, sizeof(join));
            ESP_LOGI(TAG, "Sent join request to host");
        }
    } else if (g_doom_espnow.state == STATE_HOST_WAITING && g_doom_espnow.is_host) {
        // Host received discovery - another device looking for host
        ESP_LOGI(TAG, "*** Host: device found, sending response ***");
        // Send host announcement again so the other device knows we're here
        discovery_packet_t disc;
        memcpy(disc.magic, "DOOM_DISC", 8);
        memcpy(disc.mac, g_doom_espnow.my_mac, 6);
        disc.player_num = 0;  // Host!
        disc.num_players = g_doom_espnow.players_found;
        disc.state = STATE_HOST_WAITING;

        doom_espnow_send(src_addr, &disc, sizeof(disc));
    }
}

// Process join packet from client
static void process_join(const uint8_t *src_addr, const uint8_t *data, int len) {
    ESP_LOGI(TAG, "RX join from %02x:%02x:%02x:%02x:%02x:%02x",
             src_addr[0], src_addr[1], src_addr[2], src_addr[3], src_addr[4], src_addr[5]);

    if (len < sizeof(join_packet_t)) return;

    join_packet_t *pkt = (join_packet_t*)data;

    if (memcmp(pkt->magic, "DOOM_JOIN", 8) != 0) return;
    if (!g_doom_espnow.is_host) return;  // Only host processes joins

    ESP_LOGI(TAG, "*** Join request from client! ***");

    // Add client as player 1
    doom_espnow_add_player(1, src_addr);
    g_doom_espnow.players_found = 2;  // Host + 1 client
    g_doom_espnow.state = STATE_READY;
    g_doom_espnow.state_enter_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "*** Game ready! 2 players connected. ***");
}

// Background task to handle discovery and state machine
static void doom_espnow_task(void *pvParameters) {
    TickType_t last_discovery = 0;
    TickType_t last_log = 0;
    TickType_t start_time = xTaskGetTickCount();
    bool ready_announced = false;

    g_doom_espnow.state_enter_time = start_time;

    ESP_LOGI(TAG, "ESP-NOW task started");

    // Add broadcast peer explicitly
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    while (1) {
        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed_ms = (now - start_time) * portTICK_PERIOD_MS;

        // Log status every 2 seconds
        if ((now - last_log) * portTICK_PERIOD_MS >= 2000) {
            ESP_LOGI(TAG, "State: %d, elapsed: %lu ms, players: %d",
                     g_doom_espnow.state, elapsed_ms, g_doom_espnow.players_found);
            last_log = now;
        }

        // Process incoming packets
        uint8_t src_addr[6];
        uint8_t data[250];
        int len = doom_espnow_recv(src_addr, data, sizeof(data), 0);

        if (len > 0) {
            ESP_LOGI(TAG, "Got packet: %d bytes", len);
            // Check packet type
            if (len >= 8) {
                if (memcmp(data, "DOOM_DISC", 8) == 0) {
                    process_discovery(src_addr, data, len);
                } else if (memcmp(data, "DOOM_JOIN", 8) == 0) {
                    process_join(src_addr, data, len);
                } else if (memcmp(data, "DOOM_READY", 8) == 0) {
                    ready_packet_t *pkt = (ready_packet_t*)data;
                    ESP_LOGI(TAG, "Player %d ready", pkt->player_num);
                } else {
                    ESP_LOGW(TAG, "Unknown packet: %.8s", data);
                }
            }
        }

        // State machine
        switch (g_doom_espnow.state) {
            case STATE_DISCOVERING:
                // Broadcast discovery periodically
                if ((now - last_discovery) * portTICK_PERIOD_MS >= DISCOVERY_INTERVAL_MS) {
                    discovery_packet_t disc;
                    memcpy(disc.magic, "DOOM_DISC", 8);
                    memcpy(disc.mac, g_doom_espnow.my_mac, 6);
                    disc.player_num = 255;  // Unknown
                    disc.num_players = 0;
                    disc.state = STATE_DISCOVERING;

                    doom_espnow_send(NULL, &disc, sizeof(disc));
                    last_discovery = now;
                    ESP_LOGD(TAG, "TX discovery broadcast");
                }

                // Timeout - become host
                if (elapsed_ms >= DISCOVERY_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "*** No host found. Becoming HOST... ***");
                    g_doom_espnow.is_host = true;
                    g_doom_espnow.my_player_num = 0;
                    g_doom_espnow.state = STATE_HOST_WAITING;
                    g_doom_espnow.players_found = 1;  // Just us
                    g_doom_espnow.state_enter_time = now;

                    // Send host announcement immediately
                    discovery_packet_t disc;
                    memcpy(disc.magic, "DOOM_DISC", 8);
                    memcpy(disc.mac, g_doom_espnow.my_mac, 6);
                    disc.player_num = 0;  // Host!
                    disc.num_players = 1;
                    disc.state = STATE_HOST_WAITING;

                    doom_espnow_send(NULL, &disc, sizeof(disc));
                    ESP_LOGI(TAG, "TX host announcement");
                }
                break;

            case STATE_HOST_WAITING: {
                uint32_t waiting_ms = (now - g_doom_espnow.state_enter_time) * portTICK_PERIOD_MS;

                // Keep announcing host presence
                if ((now - last_discovery) * portTICK_PERIOD_MS >= DISCOVERY_INTERVAL_MS) {
                    discovery_packet_t disc;
                    memcpy(disc.magic, "DOOM_DISC", 8);
                    memcpy(disc.mac, g_doom_espnow.my_mac, 6);
                    disc.player_num = 0;
                    disc.num_players = g_doom_espnow.players_found;
                    disc.state = STATE_HOST_WAITING;

                    doom_espnow_send(NULL, &disc, sizeof(disc));
                    last_discovery = now;
                    ESP_LOGD(TAG, "TX host announcement (waiting %lu ms)", waiting_ms);
                }

                // Check if client joined
                if (g_doom_espnow.players_found >= 2) {
                    ESP_LOGI(TAG, "*** Client joined! Starting 2-player game... ***");
                    g_doom_espnow.state = STATE_READY;
                    g_doom_espnow.state_enter_time = now;
                    ready_announced = false;
                }
                // Timeout - start single-player mode
                else if (waiting_ms >= HOST_WAITING_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "*** No client joined. Starting single-player mode... ***");
                    g_doom_espnow.state = STATE_READY;
                    g_doom_espnow.state_enter_time = now;
                    ready_announced = false;
                }
                break;
            }

            case STATE_CLIENT_JOINING: {
                uint32_t joining_ms = (now - g_doom_espnow.state_enter_time) * portTICK_PERIOD_MS;

                // Send join request periodically
                if ((now - last_discovery) * portTICK_PERIOD_MS >= DISCOVERY_INTERVAL_MS) {
                    join_packet_t join;
                    memcpy(join.magic, "DOOM_JOIN", 8);
                    memcpy(join.mac, g_doom_espnow.my_mac, 6);
                    doom_espnow_send(g_doom_espnow.host_mac, &join, sizeof(join));
                    last_discovery = now;
                    ESP_LOGD(TAG, "TX join request (waiting %lu ms)", joining_ms);
                }

                // Check if host acknowledged by sending a discovery packet
                // This would be caught in process_discovery

                // Timeout
                if (joining_ms >= JOIN_TIMEOUT_MS) {
                    ESP_LOGE(TAG, "*** Join timeout. Starting single-player... ***");
                    g_doom_espnow.is_host = true;
                    g_doom_espnow.my_player_num = 0;
                    g_doom_espnow.state = STATE_READY;
                    g_doom_espnow.state_enter_time = now;
                    ready_announced = false;
                }
                break;
            }

            case STATE_READY:
                // Announce ready once
                if (!ready_announced) {
                    ready_packet_t ready;
                    memcpy(ready.magic, "DOOM_READY", 8);
                    memcpy(ready.mac, g_doom_espnow.my_mac, 6);
                    ready.player_num = g_doom_espnow.my_player_num;

                    if (g_doom_espnow.is_host) {
                        doom_espnow_send(NULL, &ready, sizeof(ready));
                    } else {
                        doom_espnow_send(g_doom_espnow.host_mac, &ready, sizeof(ready));
                    }

                    ready_announced = true;
                    ESP_LOGI(TAG, "*** Game ready! Starting in %d ms... ***", GAME_START_DELAY_MS);
                }

                // Wait a bit then exit to let game start
                uint32_t ready_ms = (now - g_doom_espnow.state_enter_time) * portTICK_PERIOD_MS;
                if (ready_ms >= GAME_START_DELAY_MS) {
                    ESP_LOGI(TAG, "*** Starting game as %s, player %d ***",
                             g_doom_espnow.is_host ? "HOST" : "CLIENT",
                             g_doom_espnow.my_player_num);
                    return;  // Exit task, game can start
                }
                break;

            case STATE_ERROR:
                ESP_LOGE(TAG, "Error state");
                return;

            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Public API
int doom_espnow_setup(void) {
    // Get MAC address
    esp_read_mac(g_doom_espnow.my_mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "*** ESP-NOW Setup ***");
    ESP_LOGI(TAG, "My MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             g_doom_espnow.my_mac[0], g_doom_espnow.my_mac[1],
             g_doom_espnow.my_mac[2], g_doom_espnow.my_mac[3],
             g_doom_espnow.my_mac[4], g_doom_espnow.my_mac[5]);

    // Initialize ESP-NOW
    esp_err_t ret = doom_espnow_init(false);  // Don't know role yet
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed: %d", ret);
        return -1;
    }

    // Start in discovering state
    g_doom_espnow.state = STATE_DISCOVERING;
    g_doom_espnow.is_host = false;
    g_doom_espnow.my_player_num = -1;
    g_doom_espnow.players_found = 0;
    memset(g_doom_espnow.host_mac, 0, 6);

    // Run discovery task
    doom_espnow_task(NULL);

    // Return results
    ESP_LOGI(TAG, "*** Setup complete: is_host=%d, player_num=%d ***",
             g_doom_espnow.is_host, g_doom_espnow.my_player_num);

    return g_doom_espnow.is_host ? 0 : 1;  // 0 for host, 1 for client
}

bool doom_espnow_is_host(void) {
    return g_doom_espnow.is_host;
}

int doom_espnow_get_player_num(void) {
    return g_doom_espnow.my_player_num;
}

// Check if multiplayer (2 players connected)
bool doom_espnow_is_multiplayer(void) {
    return g_doom_espnow.players_found >= 2;
}
