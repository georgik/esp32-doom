// ESP-NOW network wrapper for Doom multiplayer
#include "espnow_network.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "protocol.h"
#endif

static const char *TAG = "doom_espnow";

static espnow_state_t g_espnow = {0};
static SemaphoreHandle_t g_espnow_mutex = NULL;
static QueueHandle_t g_recv_queue = NULL;
static bool g_wifi_initialized = false;
static volatile int g_espnow_rx_count = 0;  // Total packets received by ESP-NOW
static volatile int g_espnow_tx_count = 0;  // Total packets sent via ESP-NOW
static volatile bool g_send_in_progress = false;  // Flow control: wait for callback

#define RECV_QUEUE_SIZE 16
#define MAX_PACKET_SIZE 250

typedef struct {
    uint8_t addr[6];
    uint8_t data[MAX_PACKET_SIZE];
    size_t len;
} espnow_rx_packet_t;

// Forward declarations
static esp_err_t espnow_add_peer(const uint8_t *mac_addr);

// Initialize WiFi (required for ESP-NOW)
static esp_err_t init_wifi(void) {
    if (g_wifi_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi...");

    // Initialize NVS (required for WiFi PHY calibration)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TCP/IP stack (needed for WiFi)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create WiFi station interface
    esp_netif_create_default_wifi_sta();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;  // Disable NVS for faster startup

    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %d", ret);
        return ret;
    }

    // Use RAM storage for faster ESP-NOW operation
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Set WiFi mode to STA
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed: %d", ret);
        return ret;
    }

    // Start WiFi (doesn't need to connect to AP, just be running)
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %d", ret);
        return ret;
    }

    // Set WiFi channel to 1 for ESP-NOW (both devices must be on same channel)
    ret = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set channel failed: %d", ret);
        return ret;
    }

    // Enable promiscuous mode to prevent AP scanning
    // When STA mode is not connected, it scans for APs which blocks ESP-NOW RX
    ret = esp_wifi_set_promiscuous(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi promiscuous mode failed: %d", ret);
    }

    g_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized on channel 1, promiscuous mode enabled");
    return ESP_OK;
}

// Send ACK for tic packets - must be fast (called from ISR context)
static void IRAM_ATTR send_ack_from_isr(const uint8_t *dest_addr) {
    packet_header_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.type = PKT_ACK;
    ack.reserved[0] = 0;
    ack.reserved[1] = 0;
    ack.tic = 0;

    esp_now_send(dest_addr, (uint8_t*)&ack, sizeof(ack));
}

// Data receive callback - called from ESP-NOW task
static void IRAM_ATTR espnow_data_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len > MAX_PACKET_SIZE) return;

    const uint8_t *mac_addr = recv_info->src_addr;
    g_espnow_rx_count++;

    // DISABLED: ACK mechanism was causing TX queue overflow
    // Using fire-and-forget with duplicate detection in I_GetPacket instead
    // if (len >= sizeof(packet_header_t)) {
    //     const packet_header_t *pkt = (const packet_header_t*)data;
    //     if (pkt->type == PKT_TICC || pkt->type == PKT_TICS) {
    //         send_ack_from_isr(mac_addr);
    //     }
    // }

    if (g_recv_queue) {
        espnow_rx_packet_t pkt;
        memcpy(pkt.addr, mac_addr, 6);
        memcpy(pkt.data, data, len);
        pkt.len = len;

        BaseType_t ret = xQueueSendFromISR(g_recv_queue, &pkt, 0);
        if (ret != pdTRUE) {
            // Queue full - packet dropped
        }
    }
}

// Send callback - called from WiFi task (high priority)
// Must be fast - no logging, just count and clear flag
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    g_espnow_tx_count++;
    g_send_in_progress = false;  // Ready for next send
}

static esp_err_t espnow_add_peer(const uint8_t *mac_addr) {
    // Check if peer already exists
    esp_now_peer_info_t peer_info;
    if (esp_now_get_peer(mac_addr, &peer_info) == ESP_OK) {
        // Peer already exists, no need to modify
        return ESP_OK;
    }

    // Add new peer
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac_addr, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Failed to add peer %02x:%02x:%02x:%02x:%02x:%02x: %d",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5], ret);
    } else {
        ESP_LOGI(TAG, "Peer added %02x:%02x:%02x:%02x:%02x:%02x: %d",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5], ret);
    }
    return ret;
}

esp_err_t doom_espnow_init(bool as_host) {
    if (g_espnow.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Initialize WiFi first (required for ESP-NOW)
    esp_err_t ret = init_wifi();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialization failed: %d", ret);
        return ret;
    }

    g_espnow_mutex = xSemaphoreCreateMutex();
    g_recv_queue = xQueueCreate(RECV_QUEUE_SIZE, sizeof(espnow_rx_packet_t));

    if (!g_espnow_mutex || !g_recv_queue) {
        ESP_LOGE(TAG, "Failed to create mutex/queue");
        if (g_espnow_mutex) vSemaphoreDelete(g_espnow_mutex);
        if (g_recv_queue) vQueueDelete(g_recv_queue);
        return ESP_ERR_NO_MEM;
    }

    // Get MAC address
    esp_read_mac(g_espnow.my_addr, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "My MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             g_espnow.my_addr[0], g_espnow.my_addr[1], g_espnow.my_addr[2],
             g_espnow.my_addr[3], g_espnow.my_addr[4], g_espnow.my_addr[5]);

    // Initialize ESP-NOW
    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %d", ret);
        vSemaphoreDelete(g_espnow_mutex);
        vQueueDelete(g_recv_queue);
        return ret;
    }

    // Register receive callback
    ret = esp_now_register_recv_cb(espnow_data_recv_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %d", ret);
        esp_now_deinit();
        vSemaphoreDelete(g_espnow_mutex);
        vQueueDelete(g_recv_queue);
        return ret;
    }
    ESP_LOGI(TAG, "Receive callback registered");

    // Register send callback
    ret = esp_now_register_send_cb(espnow_send_cb);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_register_send_cb failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Send callback registered");
    }

    // Register broadcast peer explicitly for ESP-NOW
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ret = espnow_add_peer(broadcast_mac);
    ESP_LOGI(TAG, "Broadcast peer registered: %d", ret);

    // Short wait for ESP-NOW to stabilize (promiscuous mode prevents AP scanning)
    ESP_LOGI(TAG, "Waiting for ESP-NOW to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(500));  // Reduced from 2s - promiscuous mode helps

    g_espnow.is_host = as_host;
    g_espnow.initialized = true;

    // Host is always player 0
    if (as_host) {
        g_espnow.num_players = 1;
        memcpy(g_espnow.players[0].addr, g_espnow.my_addr, 6);
        g_espnow.players[0].connected = true;
        g_espnow.players[0].player_num = 0;
    }

    ESP_LOGI(TAG, "ESP-NOW initialized as %s", as_host ? "HOST" : "CLIENT");
    return ESP_OK;
}

esp_err_t doom_espnow_deinit(void) {
    if (!g_espnow.initialized) return ESP_OK;

    esp_now_deinit();

    if (g_wifi_initialized) {
        esp_wifi_stop();
        esp_wifi_deinit();
        g_wifi_initialized = false;
    }

    if (g_espnow_mutex) vSemaphoreDelete(g_espnow_mutex);
    if (g_recv_queue) vQueueDelete(g_recv_queue);

    memset(&g_espnow, 0, sizeof(g_espnow));
    return ESP_OK;
}

esp_err_t doom_espnow_send(const uint8_t *dest_addr, const void *data, size_t len) {
    if (!g_espnow.initialized) return ESP_ERR_INVALID_STATE;

    uint8_t dest[6];
    bool is_broadcast = false;

    if (dest_addr == NULL) {
        // Broadcast to all
        memset(dest, 0xFF, 6);
        is_broadcast = true;
    } else {
        memcpy(dest, dest_addr, 6);
    }

    // Only add/modify peer if not already present (check first)
    esp_now_peer_info_t peer_info;
    if (esp_now_get_peer(dest, &peer_info) != ESP_OK) {
        // Peer doesn't exist, add it
        if (is_broadcast) {
            memset(dest, 0xFF, 6);
        }
        espnow_add_peer(dest);
    }

    // Flow control: check if previous send is still pending
    // If WiFi task is starved, callback may never fire - don't wait indefinitely
    if (g_send_in_progress) {
        // Previous send not complete - TX queue likely full or WiFi task starved
        return ESP_ERR_TIMEOUT;
    }

    // Mark send as in-progress
    g_send_in_progress = true;

    // Log occasionally
    static int actual_send_count = 0;
    if ((actual_send_count++ % 100) == 0) {
        ESP_LOGI(TAG, "esp_now_send: count=%d tx_cb=%d",
                 actual_send_count, g_espnow_tx_count);
    }

    esp_err_t ret = esp_now_send(dest, data, len);
    if (ret != ESP_OK) {
        // Send failed immediately - clear flag
        g_send_in_progress = false;
        // Rate limit error logging to avoid spam
        static TickType_t last_error_time = 0;
        TickType_t now = xTaskGetTickCount();
        if (last_error_time == 0 || (now - last_error_time) * portTICK_PERIOD_MS > 1000) {
            ESP_LOGW(TAG, "esp_now_send failed: %d", ret);
            last_error_time = now;
        }
    }
    return ret;
}

// Receive packet with timeout
int doom_espnow_recv(uint8_t *src_addr, void *data, size_t max_len, int timeout_ms) {
    if (!g_espnow.initialized || !g_recv_queue) return -1;

    espnow_rx_packet_t pkt;
    BaseType_t ret;

    if (timeout_ms < 0) {
        // Wait forever
        ret = xQueueReceive(g_recv_queue, &pkt, portMAX_DELAY);
    } else if (timeout_ms == 0) {
        // Non-blocking
        ret = xQueueReceive(g_recv_queue, &pkt, 0);
    } else {
        // Timeout
        ret = xQueueReceive(g_recv_queue, &pkt, pdMS_TO_TICKS(timeout_ms));
    }

    if (ret == pdTRUE) {
        if (src_addr) memcpy(src_addr, pkt.addr, 6);
        size_t copy_len = (pkt.len < max_len) ? pkt.len : max_len;
        memcpy(data, pkt.data, copy_len);
        return copy_len;
    }

    return -1; // Timeout or error
}

// Peek at packet type without consuming from queue
// Returns packet type (byte 0-7) or -1 if no packet available
int doom_espnow_peek_type(void) {
    if (!g_espnow.initialized || !g_recv_queue) return -1;

    espnow_rx_packet_t pkt;
    BaseType_t ret = xQueuePeek(g_recv_queue, &pkt, 0);

    if (ret == pdTRUE && pkt.len >= 1) {
        // Check packet type from magic or first byte
        // For Doom packets, type is at offset 8 (after header)
        // For discovery packets, check magic
        if (pkt.len >= 8 && memcmp(pkt.data, "DOOM_", 5) == 0) {
            return 255;  // Special value for ESP-NOW setup packets
        }
        if (pkt.len >= sizeof(packet_header_t)) {
            packet_header_t *header = (packet_header_t*)pkt.data;
            return header->type;
        }
    }
    return -1;
}

// Check if data is available
bool doom_espnow_data_available(void) {
    if (!g_recv_queue) return false;
    return uxQueueMessagesWaiting(g_recv_queue) > 0;
}

const espnow_state_t* doom_espnow_get_state(void) {
    return &g_espnow;
}

esp_err_t doom_espnow_start_discovery(void) {
    uint8_t discovery[32] = "DOOM_DISCOVER";
    return doom_espnow_send(NULL, discovery, sizeof(discovery));
}

esp_err_t doom_espnow_join_game(const uint8_t *host_addr) {
    uint8_t join_req[32];
    memcpy(join_req, "DOOM_JOIN", 9);
    memcpy(join_req + 9, g_espnow.my_addr, 6);
    return doom_espnow_send(host_addr, join_req, 9 + 6);
}

bool doom_espnow_is_initialized(void) {
    return g_espnow.initialized;
}

// Add/update player in state
esp_err_t doom_espnow_add_player(int player_num, const uint8_t *addr) {
    if (player_num < 0 || player_num >= ESPNOW_MAX_PLAYERS) {
        ESP_LOGE(TAG, "add_player: invalid player_num %d", player_num);
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(g_espnow.players[player_num].addr, addr, 6);
    g_espnow.players[player_num].connected = true;
    g_espnow.players[player_num].player_num = player_num;

    if (player_num + 1 > g_espnow.num_players) {
        g_espnow.num_players = player_num + 1;
    }

    ESP_LOGI(TAG, "Player %d added: %02x:%02x:%02x:%02x:%02x:%02x, num_players=%d",
             player_num, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], g_espnow.num_players);

    return ESP_OK;
}

// Send to specific player
esp_err_t doom_espnow_send_to_player(int player_num, const void *data, size_t len) {
    if (player_num < 0 || player_num >= ESPNOW_MAX_PLAYERS) {
        ESP_LOGW(TAG, "send_to_player: invalid player_num %d", player_num);
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_espnow.players[player_num].connected) {
        ESP_LOGW(TAG, "send_to_player: player %d not connected", player_num);
        return ESP_ERR_INVALID_ARG;
    }

    // Reduced logging - only log every 100th call
    static int send_to_player_count = 0;
    if ((++send_to_player_count % 100) == 0) {
        ESP_LOGI(TAG, "send_to_player: count=%d to player %d", send_to_player_count, player_num);
    }

    return doom_espnow_send(g_espnow.players[player_num].addr, data, len);
}

// Get ESP-NOW statistics
void doom_espnow_get_stats(int *tx_count, int *rx_count, int *dropped_count) {
    if (tx_count) *tx_count = g_espnow_tx_count;
    if (rx_count) *rx_count = g_espnow_rx_count;
    if (dropped_count) *dropped_count = 0;  // No flow control, no drops
}

// Check if TX is busy (previous send not completed)
bool doom_espnow_tx_busy(void) {
    return g_send_in_progress;
}

// ticcmd_t is 6 bytes: forwardmove(1), sidemove(1), angleturn(2), consistancy(2), chatchar(1), buttons(1)
#define TICCMD_SIZE 6

// Global sequence counter for tics packets
static volatile uint16_t g_tic_seq_num = 0;

// Send ticcmd packet to specific player
esp_err_t doom_espnow_send_tics(int player_num, uint32_t start_tic,
                                const void *tics, int count) {
    if (!g_espnow.initialized) return ESP_ERR_INVALID_STATE;
    if (player_num < 0 || player_num >= ESPNOW_MAX_PLAYERS) return ESP_ERR_INVALID_ARG;
    if (!g_espnow.players[player_num].connected) return ESP_ERR_INVALID_ARG;
    if (count <= 0 || count > 35) return ESP_ERR_INVALID_ARG;
    if (!tics) return ESP_ERR_INVALID_ARG;

    // Build packet with variable tic data (with seq_num and timestamp)
    struct {
        packet_header_t header;
        byte sendtics;
        byte player_num;
        uint16_t seq_num;
        uint32_t timestamp;
        byte ticdata[TICCMD_SIZE * 35];
    } __attribute__((packed)) pkt;

    memset(&pkt, 0, sizeof(pkt));
    packet_set(&pkt.header, PKT_TICC, start_tic);
    pkt.sendtics = count;
    pkt.player_num = g_espnow.is_host ? 0 : 1;
    pkt.seq_num = __sync_fetch_and_add(&g_tic_seq_num, 1);
    pkt.timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
    memcpy(pkt.ticdata, tics, TICCMD_SIZE * count);

    int size = sizeof(packet_header_t) + 2 + sizeof(uint16_t) + sizeof(uint32_t) + TICCMD_SIZE * count;
    return doom_espnow_send_to_player(player_num, &pkt, size);
}
