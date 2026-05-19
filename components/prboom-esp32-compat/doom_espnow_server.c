// Doom ESP-NOW Server Protocol Implementation
#include "doom_espnow_server.h"
#include "doom_espnow.h"
#include "espnow_network.h"
#include "esp_log.h"
#include "protocol.h"
#include "doomstat.h"
#include "d_ticcmd.h"
#include "d_net.h"
#include "m_swap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "doom_server";

// Half-duplex timing configuration - smaller packets more frequently to avoid NO_MEM
#define TICS_PER_SEND 5           // ~140ms of gameplay at 35Hz (was 17)
#define SEND_INTERVAL_PERIODS 1   // Send every 1 period (1 second) - was 2
#define HALF_DUPLEX_PERIOD_MS 1000 // Full period (host + client)
#define HOST_SEND_START_MS 0      // Host: 0-200ms (smaller window)
#define HOST_SEND_END_MS 200      // Reduced from 350ms
#define CLIENT_SEND_START_MS 500  // Client: 500-700ms (smaller window)
#define CLIENT_SEND_END_MS 700    // Reduced from 850ms
#define GUARD_GAP_MS 300          // Gap between windows (200-500, 700-1000)

// Server state
typedef struct {
    bool is_server;
    bool client_connected;
    bool game_started;
    int my_player_num;
    doom_server_config_t config;

    // Client address
    uint8_t client_mac[6];

    // Send tracking
    uint32_t last_send_tic;        // Last tic number sent
    uint32_t last_send_period;     // Period number when we last attempted send
    bool waiting_for_ack;          // Waiting for ACK before next send
    uint32_t ack_timeout_period;   // Period when ACK will timeout
    bool send_callback_fired;      // Track if send callback completed
    int nomem_failures;            // Consecutive NO_MEM failure count
    int backoff_periods;           // Extra periods to skip due to backoff

    // Half-duplex timing
    uint32_t game_start_time;      // TickCount when game started
} doom_server_t;

static doom_server_t g_server = {0};

// Sequence number tracking (monotonically increasing)
static volatile uint16_t g_send_seq_num = 0;

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
    g_server.last_send_tic = 0;
    g_server.last_send_period = 0;
    g_server.waiting_for_ack = false;

    // Client waits for GO packet before starting
    if (!g_server.is_server && g_server.client_connected) {
        ESP_LOGI(TAG, "Client: waiting for GO packet...");
    }
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

// Send GO packet with game start delay (relative, not absolute TickCount)
static void send_go_packet(void) {
    struct {
        packet_header_t header;
        uint32_t start_delay_ms;  // Milliseconds from NOW when game starts
    } __attribute__((packed)) go_pkt;

    memset(&go_pkt, 0, sizeof(go_pkt));
    packet_set(&go_pkt.header, PKT_GO, 0);
    go_pkt.start_delay_ms = 2000;  // Start 2 seconds from receiving GO packet

    ESP_LOGI(TAG, "Sending PKT_GO to client with start_delay_ms=%lu", go_pkt.start_delay_ms);
    doom_espnow_send_to_player(1, &go_pkt, sizeof(go_pkt));
}

// Send tics using half-duplex timing
// Host sends at 0-200ms, 1000-1200ms, 2000-2200ms...
// Client sends at 400-600ms, 1400-1600ms, 2400-2600ms...
static void send_tics(void) {
    static int send_call_count = 0;
    if ((send_call_count++ % 200) == 0) {
        ESP_LOGI(TAG, "send_tics: calls=%d connected=%d started=%d",
                 send_call_count, g_server.client_connected, g_server.game_started);
    }

    if (!g_server.client_connected || !g_server.game_started) return;

    extern int maketic;
    extern ticcmd_t netcmds[MAXPLAYERS][BACKUPTICS];
    extern bool doom_espnow_tx_busy(void);

    uint32_t now = xTaskGetTickCount();

    // Don't send before game start time
    if (now < g_server.game_start_time) {
        static int early_return_count = 0;
        if ((early_return_count++ % 200) == 0) {
            ESP_LOGI(TAG, "send_tics: early return now=%lu start=%lu",
                     now, g_server.game_start_time);
        }
        return;  // Game hasn't started yet
    }

    uint32_t elapsed_ms = (now - g_server.game_start_time) * portTICK_PERIOD_MS;

    // Calculate current period (0, 1, 2, ...)
    uint32_t current_period = elapsed_ms / HALF_DUPLEX_PERIOD_MS;

    // DISABLED: Stop-and-wait with ACK - was causing TX queue overflow
    // Now using fire-and-forget with duplicate detection in receiver
    // if (g_server.waiting_for_ack && current_period >= g_server.ack_timeout_period) {
    //     ESP_LOGW(TAG, "ACK timeout, clearing wait flag");
    //     g_server.waiting_for_ack = false;
    // }
    // if (g_server.waiting_for_ack) {
    //     return;
    // }

    // DISABLED: TX queue drain - not needed for fire-and-forget
    // static bool first_send = true;
    // if (first_send) {
    //     int drain_count = 0;
    //     while (doom_espnow_tx_busy() && drain_count < 100) {
    //         vTaskDelay(pdMS_TO_TICKS(10));
    //         drain_count++;
    //     }
    //     ESP_LOGI(TAG, "TX drain: %d iterations, busy=%d", drain_count, doom_espnow_tx_busy());
    //     first_send = false;
    // }

    // DISABLED: TX busy check - not needed for fire-and-forget
    // if (doom_espnow_tx_busy()) {
    //     return;  // Previous send still in progress
    // }

    // Only send every N periods to reduce load on WiFi task
    // Check against last ATTEMPT period (not just successful sends) to prevent retry storms
    // Also include backoff if we've had NO_MEM failures
    uint32_t min_periods = SEND_INTERVAL_PERIODS + g_server.backoff_periods;
    if (current_period < g_server.last_send_period + min_periods) {
        return;  // Too soon since last send attempt
    }

    // Calculate phase within the 1000ms period
    uint32_t phase_ms = elapsed_ms % HALF_DUPLEX_PERIOD_MS;

    // Log phase occasionally (every 200 calls = every 2 seconds)
    static int phase_log_count = 0;
    if ((phase_log_count++ % 200) == 0) {
        ESP_LOGI(TAG, "send_tics: phase=%lu ms, is_server=%d, window: %d-%d ms",
                 phase_ms, g_server.is_server,
                 g_server.is_server ? HOST_SEND_START_MS : CLIENT_SEND_START_MS,
                 g_server.is_server ? HOST_SEND_END_MS : CLIENT_SEND_END_MS);
    }

    // Check if in send window with guard gap to prevent drift overlap
    bool in_window = g_server.is_server
        ? (phase_ms >= HOST_SEND_START_MS && phase_ms < HOST_SEND_END_MS)
        : (phase_ms >= CLIENT_SEND_START_MS && phase_ms < CLIENT_SEND_END_MS);

    if (!in_window) {
        return;  // Not our time window
    }

    // Mark this period as attempted BEFORE checking other conditions
    // This prevents multiple attempts within the same window even if sends fail
    g_server.last_send_period = current_period;

    // Count tics to send (max TICS_PER_SEND)
    int tics_to_send = maketic - g_server.last_send_tic;
    if (tics_to_send <= 0) return;
    if (tics_to_send > TICS_PER_SEND) tics_to_send = TICS_PER_SEND;

    ESP_LOGI(TAG, "Send: tics=%d start=%lu player=%d phase=%lu",
             tics_to_send, g_server.last_send_tic, g_server.my_player_num, phase_ms);

    // Build packet with tics
    struct {
        packet_header_t header;
        byte sendtics;
        byte player_num;
        uint16_t seq_num;
        uint32_t timestamp;
        ticcmd_t ticdata[TICS_PER_SEND];
    } __attribute__((packed)) tic_pkt;

    memset(&tic_pkt, 0, sizeof(tic_pkt));
    packet_set(&tic_pkt.header, PKT_TICC, g_server.last_send_tic);
    tic_pkt.sendtics = tics_to_send;
    tic_pkt.player_num = g_server.my_player_num;
    tic_pkt.seq_num = __sync_fetch_and_add(&g_send_seq_num, 1);
    tic_pkt.timestamp = now;

    // Copy tics from netcmds (handle wraparound)
    for (int i = 0; i < tics_to_send; i++) {
        int tic_idx = (g_server.last_send_tic + i) % BACKUPTICS;
        tic_pkt.ticdata[i] = netcmds[g_server.my_player_num][tic_idx];
    }

    int pkt_size = sizeof(packet_header_t) + 2 + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(ticcmd_t) * tics_to_send;

    // Log every send
    ESP_LOGI(TAG, "Send: tics=%d start=%lu player=%d phase=%lu",
             tics_to_send, g_server.last_send_tic, g_server.my_player_num, phase_ms);

    // Send to other player
    int target_player = 1 - g_server.my_player_num;
    esp_err_t ret = doom_espnow_send_to_player(target_player, &tic_pkt, pkt_size);

    if (ret == ESP_OK) {
        g_server.last_send_tic += tics_to_send;
        g_server.nomem_failures = 0;  // Reset failure counter on success
        g_server.backoff_periods = 0;  // Reset backoff on success
    } else {
        ESP_LOGW(TAG, "Send failed: ret=%d", ret);
        // Simplified backoff: skip 1 period on NO_MEM to reduce TX queue pressure
        if (ret == 12391) {  // ESP_ERR_ESPNOW_NO_MEM
            g_server.nomem_failures++;
            g_server.backoff_periods = 1;  // Skip just 1 period (1 second)
            ESP_LOGW(TAG, "NO_MEM: skipping next send period");
        }
        // Fire-and-forget: no ACK wait, will retry next period
    }
}

// Server tick - handle network packets
void doom_server_tick(void) {
    static int tick_count = 0;
    static int last_rx_log = 0;
    tick_count++;

    // Log status every 100 ticks (1 second at 100Hz) - increased frequency for debugging
    if ((tick_count % 100) == 0) {
        int espnow_tx, espnow_rx, espnow_dropped;
        doom_espnow_get_stats(&espnow_tx, &espnow_rx, &espnow_dropped);
        ESP_LOGI(TAG, "Tick: is_server=%d connected=%d game_started=%d my_player=%d ESPNOW: TX=%d RX=%d Dropped=%d",
                 g_server.is_server, g_server.client_connected,
                 g_server.game_started, g_server.my_player_num, espnow_tx, espnow_rx, espnow_dropped);
    }

    // Check for incoming control packets only
    // Tic packets (PKT_TICC, PKT_TICS) flow to Doom's I_GetPacket via i_network.c
    uint8_t src_addr[6];
    uint8_t data[250];
    int len;

    while (doom_espnow_data_available()) {
        int pkt_type = doom_espnow_peek_type();

        // Log all packet types for debugging
        if ((tick_count % 100) == 0 && pkt_type >= 0) {
            ESP_LOGI(TAG, "Peeked packet type: %d", pkt_type);
        }

        // Only process control packets here - leave tic packets for Doom
        // PKT_ACK removed - using fire-and-forget with duplicate detection
        if (pkt_type == PKT_INIT || pkt_type == PKT_SETUP || pkt_type == PKT_GO ||
            pkt_type == PKT_QUIT || pkt_type == 255) {
            len = doom_espnow_recv(src_addr, data, sizeof(data), 0);
            if (len <= 0) break;
        } else {
            // Tic packet - leave for Doom's NetUpdate
            break;
        }

        if (len < sizeof(packet_header_t)) {
            continue;
        }

        packet_header_t *pkt = (packet_header_t*)data;

        switch (pkt->type) {
            // PKT_ACK removed - using fire-and-forget with duplicate detection

            case PKT_INIT:
                ESP_LOGI(TAG, "PKT_INIT received: is_server=%d game_started=%d",
                         g_server.is_server, g_server.game_started);
                if (g_server.is_server && !g_server.game_started) {
                    // Host: start 2 seconds from now
                    g_server.game_start_time = xTaskGetTickCount() + pdMS_TO_TICKS(2000);

                    send_setup_packet();
                    send_go_packet();
                    g_server.game_started = true;

                    ESP_LOGI(TAG, "Setup sent, game starts in 2s");
                }
                break;

            case PKT_SETUP:
                // Client: received game setup from host
                ESP_LOGI(TAG, "PKT_SETUP received from host");
                // Setup parameters are already configured, just acknowledge
                break;

            case PKT_GO: {
                ESP_LOGI(TAG, "PKT_GO received! is_server=%d len=%d", g_server.is_server, len);
                g_server.game_started = true;
                g_server.client_connected = true;

                // Client: extract delay from GO packet and calculate local start time
                if (!g_server.is_server && len >= sizeof(packet_header_t) + sizeof(uint32_t)) {
                    struct {
                        packet_header_t header;
                        uint32_t start_delay_ms;
                    } __attribute__((packed)) *go_pkt = (void*)data;

                    // Calculate game start time relative to when we received GO
                    g_server.game_start_time = xTaskGetTickCount() + pdMS_TO_TICKS(go_pkt->start_delay_ms);
                    ESP_LOGI(TAG, "CLIENT: GO received, game starts in %lu ms, start_time=%lu",
                             go_pkt->start_delay_ms, g_server.game_start_time);
                }
                break;
            }

            case PKT_QUIT:
                g_server.client_connected = false;
                g_server.game_started = false;
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

    ESP_LOGI(TAG, "Sending PKT_INIT to player 0, sizeof(init_pkt)=%d", sizeof(init_pkt));
    esp_err_t ret = doom_espnow_send_to_player(0, &init_pkt, sizeof(init_pkt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send PKT_INIT: %d", ret);
    } else {
        ESP_LOGI(TAG, "PKT_INIT sent successfully");
    }
}
