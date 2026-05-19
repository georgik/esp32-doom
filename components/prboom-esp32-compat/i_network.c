/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *
 *  ESP-NOW network implementation for ESP32
 *
 *-----------------------------------------------------------------------------*/

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef HAVE_NET

#include "protocol.h"
#include "i_network.h"
#include "lprintf.h"
#include "esp_log.h"
#include "doom_espnow_server.h"
#include "doom_espnow.h"
#include "espnow_network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "doom_net";

#define MAX_PLAYERS 4

typedef struct {
    uint8_t mac_addr[6];
    int player_num;
    bool active;
} player_channel_t;

static player_channel_t g_players[MAX_PLAYERS];
static int g_my_player_num = 0;
static bool g_network_initialized = false;
static bool g_is_server = false;

// Sequence number tracking for duplicate detection
static uint16_t g_last_seen_seq[MAX_PLAYERS] = {0};

// UDP_CHANNEL maps to player index for ESP-NOW
typedef int UDP_CHANNEL;

#define CHAN_INVALID -1
#define CHAN_BROADCAST -2

static UDP_CHANNEL sentfrom = CHAN_INVALID;
static size_t sentbytes = 0, recvdbytes = 0;
int udp_socket = 0;

void I_InitNetwork(void)
{
    if (g_network_initialized) {
        ESP_LOGW(TAG, "Network already initialized");
        return;
    }

    ESP_LOGI(TAG, "Initializing ESP-NOW network...");

    g_is_server = doom_espnow_is_host();

    esp_err_t ret = doom_espnow_init(g_is_server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ESP-NOW: %d", ret);
        return;
    }

    memset(g_players, 0, sizeof(g_players));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_players[i].player_num = CHAN_INVALID;
        g_players[i].active = false;
    }

    g_my_player_num = doom_espnow_get_player_num();

    // Get player info from ESP-NOW state
    const espnow_state_t *espnow_state = doom_espnow_get_state();
    if (espnow_state) {
        ESP_LOGI(TAG, "ESP-NOW state: num_players=%d", espnow_state->num_players);
        for (int i = 0; i < espnow_state->num_players && i < MAX_PLAYERS; i++) {
            if (espnow_state->players[i].connected) {
                g_players[i].active = true;
                g_players[i].player_num = i;
                memcpy(g_players[i].mac_addr, espnow_state->players[i].addr, 6);
                ESP_LOGI(TAG, "Player %d: %02x:%02x:%02x:%02x:%02x:%02x",
                         i, g_players[i].mac_addr[0], g_players[i].mac_addr[1],
                         g_players[i].mac_addr[2], g_players[i].mac_addr[3],
                         g_players[i].mac_addr[4], g_players[i].mac_addr[5]);
            }
        }
    } else {
        // Fallback if ESP-NOW state not available
        g_players[0].active = true;
        g_players[0].player_num = 0;
        if (doom_espnow_is_multiplayer()) {
            g_players[1].active = true;
            g_players[1].player_num = 1;
        }
    }

    g_network_initialized = true;

    ESP_LOGI(TAG, "Network initialized, I am player %d", g_my_player_num);
}

void I_ShutdownNetwork(void)
{
    if (!g_network_initialized) return;

    ESP_LOGI(TAG, "Shutting down network...");
    doom_espnow_deinit();
    g_network_initialized = false;
}

UDP_SOCKET I_Socket(Uint16 port)
{
    ESP_LOGI(TAG, "Socket requested on port %d", port);
    return (UDP_SOCKET)1;
}

void I_CloseSocket(UDP_SOCKET sock)
{
    ESP_LOGI(TAG, "Socket closed");
}

UDP_CHANNEL I_RegisterPlayer(IPaddress *ipaddr)
{
    if (!g_network_initialized) {
        ESP_LOGE(TAG, "Network not initialized");
        return CHAN_INVALID;
    }

    // Find empty slot
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_players[i].active) {
            g_players[i].player_num = i;
            g_players[i].active = true;

            ESP_LOGI(TAG, "Registered player %d", i);
            return i;
        }
    }

    ESP_LOGE(TAG, "No free player slots");
    return CHAN_INVALID;
}

void I_UnRegisterPlayer(UDP_CHANNEL channel)
{
    if (channel < 0 || channel >= MAX_PLAYERS) return;

    ESP_LOGI(TAG, "Unregistering player %d", channel);
    g_players[channel].active = false;
    g_players[channel].player_num = CHAN_INVALID;
    memset(g_players[channel].mac_addr, 0, 6);
}

int I_ConnectToServer(const char *serv)
{
    ESP_LOGI(TAG, "Connect to server: %s", serv);

    if (!g_network_initialized) {
        I_InitNetwork();
    }

    // Send init packet
    doom_client_send_init();

    return 1;
}

void I_Disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting...");
    memset(g_players, 0, sizeof(g_players));
}

UDP_PACKET *I_AllocPacket(int size)
{
    UDP_PACKET *pkt = malloc(sizeof(UDP_PACKET) + size);
    if (pkt) {
        pkt->maxlen = size;
        pkt->len = 0;
        pkt->data = (byte*)(pkt + 1);
    }
    return pkt;
}

void I_FreePacket(UDP_PACKET *packet)
{
    if (packet) {
        free(packet);
    }
}

void I_WaitForPacket(int ms)
{
    // Process server ticks while waiting
    doom_server_tick();

    if (ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

static byte ChecksumPacket(const packet_header_t* buffer, size_t len)
{
    const byte* p = (const byte*)buffer;
    byte sum = 0;

    if (len==0) return 0;

    for (size_t i = 0; i < len; i++) {
        sum += p[i];
    }

    return sum;
}

size_t I_GetPacket(packet_header_t* buffer, size_t buflen)
{
    if (!g_network_initialized) return 0;

    // Process server ticks first
    doom_server_tick();

    // Check for received packets via ESP-NOW
    uint8_t src_addr[6];
    int len = doom_espnow_recv(src_addr, buffer, buflen, 0);

    static int call_count = 0;
    if ((call_count++ % 50) == 0) {  // Log every 50 calls (more frequent)
        ESP_LOGI(TAG, "I_GetPacket: calls=%d, last_len=%d", call_count, len);
    }

    if (len > 0) {
        recvdbytes += len;

        ESP_LOGI(TAG, "I_GetPacket: got %d bytes, type=%d", len, buffer->type);

        // Duplicate detection for tic packets (PKT_TICC)
        // New packet format includes seq_num after player_num
        if (len >= sizeof(packet_header_t) + 4) {  // header + sendtics + player_num + seq_num(2)
            if (buffer->type == PKT_TICC) {
                const byte *data = (const byte*)buffer;
                byte player_num = data[sizeof(packet_header_t) + 1];  // After sendtics
                uint16_t seq_num = *(const uint16_t*)(data + sizeof(packet_header_t) + 2);  // After player_num

                // Check for duplicate (using wrapping comparison)
                if (g_players[player_num].active) {
                    int16_t seq_delta = (int16_t)(seq_num - g_last_seen_seq[player_num]);
                    if (seq_delta <= 0 && g_last_seen_seq[player_num] != 0) {
                        // Old or duplicate packet
                        ESP_LOGW(TAG, "Dropping duplicate packet: player=%d seq=%u (last=%u)",
                                 player_num, seq_num, g_last_seen_seq[player_num]);
                        return 0;
                    }
                    g_last_seen_seq[player_num] = seq_num;
                }
            }
        }

        // Find which player sent this
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (g_players[i].active &&
                memcmp(g_players[i].mac_addr, src_addr, 6) == 0) {
                sentfrom = i;
                break;
            }
        }

        ESP_LOGI(TAG, "I_GetPacket: from player %d", sentfrom);
        return len;
    }

    return 0;
}

void I_SendPacket(packet_header_t* packet, size_t len)
{
    if (!g_network_initialized) return;

    // Set checksum before sending
    packet->checksum = ChecksumPacket(packet, len);

    sentbytes += len;

    // Broadcast to all other players
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].active && i != g_my_player_num) {
            doom_espnow_send_to_player(i, packet, len);
        }
    }
}

void I_SendPacketTo(packet_header_t* packet, size_t len, UDP_CHANNEL *to)
{
    if (!g_network_initialized) return;

    packet->checksum = ChecksumPacket(packet, len);

    if (to == (UDP_CHANNEL*)CHAN_BROADCAST || (intptr_t)to == CHAN_BROADCAST) {
        // Broadcast
        doom_espnow_send(NULL, packet, len);
    } else {
        int player_idx = (int)(intptr_t)to;
        if (player_idx >= 0 && player_idx < MAX_PLAYERS && g_players[player_idx].active) {
            doom_espnow_send_to_player(player_idx, packet, len);
        }
    }

    sentbytes += len;
}

void I_PrintAddress(FILE* fp, UDP_CHANNEL *addr)
{
    if (addr == (UDP_CHANNEL*)CHAN_BROADCAST || (intptr_t)addr == CHAN_BROADCAST) {
        fprintf(fp, "BROADCAST");
    } else {
        int player_idx = (int)(intptr_t)addr;
        fprintf(fp, "PLAYER_%d", player_idx);
    }
}

#endif /* HAVE_NET */
