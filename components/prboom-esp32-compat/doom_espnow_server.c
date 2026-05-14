// Doom ESP-NOW Server Protocol Implementation
#include "doom_espnow_server.h"
#include "doom_espnow.h"
#include "espnow_network.h"
#include "esp_log.h"
#include "protocol.h"
#include "d_ticcmd.h"
#include "d_net.h"
#include "m_swap.h"
#include <string.h>

static const char *TAG = "doom_server";

#define BACKUPTICS 12

// Server state
typedef struct {
    bool is_server;
    bool client_connected;
    bool game_started;
    int my_player_num;
    doom_server_config_t config;

    // Tic data for 2 players
    ticcmd_t netcmds[MAXPLAYERS][BACKUPTICS];
    int tic_count;

    // Client address
    uint8_t client_mac[6];
} doom_server_t;

static doom_server_t g_server = {0};

// External from Doom
extern ticcmd_t netcmds[MAXPLAYERS][BACKUPTICS];
extern int maketic;
extern int ticdup;

// Initialize server
void doom_server_init(const doom_server_config_t *config) {
    memset(&g_server, 0, sizeof(g_server));

    if (doom_espnow_is_host()) {
        g_server.is_server = true;
        g_server.my_player_num = 0;
        if (config) {
            g_server.config = *config;
        } else {
            g_server.config.skill = 3;
            g_server.config.episode = 1;
            g_server.config.level = 1;
            g_server.config.deathmatch = 0;
            g_server.config.num_players = 2;
        }
        ESP_LOGI(TAG, "Server initialized: skill=%d ep=%d lvl=%d",
                 g_server.config.skill, g_server.config.episode, g_server.config.level);
    } else {
        g_server.is_server = false;
        g_server.my_player_num = 1;
        g_server.config.num_players = 2;
        ESP_LOGI(TAG, "Client initialized");

        // Store host MAC
        const espnow_state_t *state = doom_espnow_get_state();
        if (state && state->num_players >= 1) {
            memcpy(g_server.client_mac, state->players[0].addr, 6);
            ESP_LOGI(TAG, "Host MAC stored");
        }
    }

    g_server.client_connected = doom_espnow_is_multiplayer();
}

bool doom_server_has_client(void) {
    return g_server.is_server && g_server.client_connected;
}

int doom_server_get_player_num(void) {
    return g_server.my_player_num;
}

bool doom_client_connected(void) {
    return !g_server.is_server && g_server.client_connected;
}

int doom_client_get_player_num(void) {
    return g_server.my_player_num;
}

// Send setup packet (from server to client)
static void send_setup_packet(void) {
    // Build setup packet
    struct {
        packet_header_t header;
        struct setup_packet_s setup;
        char pad;  // Padding for alignment
    } __attribute__((packed)) setup_pkt;

    memset(&setup_pkt, 0, sizeof(setup_pkt));

    setup_pkt.setup.players = 2;
    setup_pkt.setup.yourplayer = 1;  // Client is player 1
    setup_pkt.setup.skill = g_server.config.skill;
    setup_pkt.setup.episode = g_server.config.episode;
    setup_pkt.setup.level = g_server.config.level;
    setup_pkt.setup.deathmatch = g_server.config.deathmatch;
    setup_pkt.setup.ticdup = 1;
    setup_pkt.setup.extratic = 0;
    setup_pkt.setup.numwads = 0;

    packet_set(&setup_pkt.header, PKT_SETUP, 0);

    ESP_LOGI(TAG, "Sending PKT_SETUP to client");
    doom_espnow_send_to_player(1, &setup_pkt, sizeof(setup_pkt));
}

// Send GO packet
static void send_go_packet(void) {
    packet_header_t go_pkt;
    memset(&go_pkt, 0, sizeof(go_pkt));
    packet_set(&go_pkt, PKT_GO, 0);

    ESP_LOGI(TAG, "Sending PKT_GO");
    doom_espnow_send(NULL, &go_pkt, sizeof(go_pkt));
}

// Send tics to other player
static void send_tics(void) {
    if (!g_server.client_connected) return;

    // Build tic packet
    struct {
        packet_header_t header;
        byte ticcmds[sizeof(ticcmd_t) * BACKUPTICS];
    } __attribute__((packed)) tic_pkt;

    memset(&tic_pkt, 0, sizeof(tic_pkt));
    packet_set(&tic_pkt.header, PKT_TICC, maketic);

    // Copy our ticcmds
    memcpy(tic_pkt.ticcmds, netcmds[g_server.my_player_num], sizeof(ticcmd_t) * BACKUPTICS);

    ESP_LOGD(TAG, "Sending %d tics", maketic);
    doom_espnow_send(NULL, &tic_pkt, sizeof(tic_pkt));
}

// Server tick - handle network packets
void doom_server_tick(void) {
    // Check for incoming packets
    uint8_t src_addr[6];
    uint8_t data[250];
    int len;

    // Process all available packets
    while ((len = doom_espnow_recv(src_addr, data, sizeof(data), 0)) > 0) {
        if (len < sizeof(packet_header_t)) continue;

        packet_header_t *pkt = (packet_header_t*)data;

        ESP_LOGD(TAG, "Got packet type=%d len=%d", pkt->type, len);

        switch (pkt->type) {
            case PKT_INIT: {
                ESP_LOGI(TAG, "PKT_INIT from client");
                // Client init - send setup
                if (g_server.is_server && !g_server.game_started) {
                    send_setup_packet();
                    send_go_packet();
                    g_server.game_started = true;
                }
                break;
            }

            case PKT_TICC: {
                // Client tics
                if (g_server.is_server && g_server.client_connected) {
                    ESP_LOGD(TAG, "PKT_TICS from client");
                    // Extract ticcmds
                    byte *tic_data = (byte*)(pkt + 1);
                    int tic_len = len - sizeof(packet_header_t);
                    int num_tics = tic_len / sizeof(ticcmd_t);

                    if (num_tics > 0 && num_tics <= BACKUPTICS) {
                        memcpy(netcmds[1], tic_data, num_tics * sizeof(ticcmd_t));
                        ESP_LOGD(TAG, "Received %d tics from client", num_tics);
                    }
                }
                break;
            }

            case PKT_TICS: {
                // Server tics (for client)
                if (!g_server.is_server && g_server.client_connected) {
                    ESP_LOGD(TAG, "PKT_TICS from server");
                    byte *tic_data = (byte*)(pkt + 1);
                    int tic_len = len - sizeof(packet_header_t);
                    int num_tics = tic_len / sizeof(ticcmd_t);

                    if (num_tics > 0 && num_tics <= BACKUPTICS) {
                        memcpy(netcmds[0], tic_data, num_tics * sizeof(ticcmd_t));
                        ESP_LOGD(TAG, "Received %d tics from server", num_tics);
                    }
                }
                break;
            }

            case PKT_GO: {
                ESP_LOGI(TAG, "PKT_GO - game starting!");
                g_server.game_started = true;
                g_server.client_connected = true;
                break;
            }

            case PKT_QUIT: {
                ESP_LOGI(TAG, "PKT_QUIT - player disconnected");
                g_server.client_connected = false;
                g_server.game_started = false;
                break;
            }

            default:
                ESP_LOGD(TAG, "Unhandled packet type: %d", pkt->type);
                break;
        }
    }

    // Send our tics if game started
    if (g_server.game_started && g_server.client_connected) {
        send_tics();
    }
}

// Send init packet (from client to server)
void doom_client_send_init(void) {
    packet_header_t init_pkt;
    memset(&init_pkt, 0, sizeof(init_pkt));
    packet_set(&init_pkt, PKT_INIT, 0);

    ESP_LOGI(TAG, "Sending PKT_INIT to server");
    doom_espnow_send_to_player(0, &init_pkt, sizeof(init_pkt));
}
