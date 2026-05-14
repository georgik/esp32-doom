// ESP-NOW network wrapper for Doom multiplayer
#include "espnow_network.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "doom_espnow";

static espnow_state_t g_espnow = {0};
static SemaphoreHandle_t g_espnow_mutex = NULL;
static QueueHandle_t g_recv_queue = NULL;
static bool g_wifi_initialized = false;

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

    g_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

// Data receive callback - called from ESP-NOW task
static void IRAM_ATTR espnow_data_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len > MAX_PACKET_SIZE) return;

    const uint8_t *mac_addr = recv_info->src_addr;

    if (g_recv_queue) {
        espnow_rx_packet_t pkt;
        memcpy(pkt.addr, mac_addr, 6);
        memcpy(pkt.data, data, len);
        pkt.len = len;

        // Try to send to queue, don't block if full
        xQueueSendFromISR(g_recv_queue, &pkt, 0);
    }
}

// Send callback
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    const uint8_t *mac_addr = tx_info ? tx_info->des_addr : NULL;
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "TX success");
    } else {
        ESP_LOGW(TAG, "TX fail: %d", status);
    }
}

static esp_err_t espnow_add_peer(const uint8_t *mac_addr) {
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac_addr, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Failed to add peer: %d", ret);
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

    // Register broadcast peer explicitly for ESP-NOW
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    espnow_add_peer(broadcast_mac);
    ESP_LOGI(TAG, "Broadcast peer registered");

    // Register send callback
    ret = esp_now_register_send_cb(espnow_send_cb);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_register_send_cb failed: %d", ret);
    }

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

    if (dest_addr == NULL) {
        // Broadcast to all
        memset(dest, 0xFF, 6);
    } else {
        memcpy(dest, dest_addr, 6);
        // Add peer if not already added
        espnow_add_peer(dest);
    }

    return esp_now_send(dest, data, len);
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
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(g_espnow.players[player_num].addr, addr, 6);
    g_espnow.players[player_num].connected = true;
    g_espnow.players[player_num].player_num = player_num;

    if (player_num + 1 > g_espnow.num_players) {
        g_espnow.num_players = player_num + 1;
    }

    ESP_LOGI(TAG, "Player %d added: %02x:%02x:%02x:%02x:%02x:%02x",
             player_num, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    return ESP_OK;
}

// Send to specific player
esp_err_t doom_espnow_send_to_player(int player_num, const void *data, size_t len) {
    if (player_num < 0 || player_num >= ESPNOW_MAX_PLAYERS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_espnow.players[player_num].connected) {
        return ESP_ERR_INVALID_ARG;
    }

    return doom_espnow_send(g_espnow.players[player_num].addr, data, len);
}
