#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOFTAP_SSID         "ESP32-InetRadio"
#define SOFTAP_IP_ADDR      "192.168.4.1"
#define MAX_SCAN_RESULTS    20

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_scan_item_t;

typedef struct {
    bool sta_connected;
    bool softap_active;
    char connected_ssid[33];
    int8_t rssi;
    char ip_addr[16];
} wifi_status_t;

typedef void (*wifi_state_cb_t)(bool sta_connected);

/**
 * @brief Initialize Wi-Fi subsystem and start initial scan/connection or SoftAP fallback
 */
esp_err_t wifi_manager_init(wifi_state_cb_t state_cb);

/**
 * @brief Trigger asynchronous rescan and connect to strongest registered AP (or fallback to SoftAP)
 */
void wifi_manager_trigger_reconnect(void);

/**
 * @brief Get current Wi-Fi status (thread-safe)
 */
void wifi_manager_get_status(wifi_status_t *out_status);

/**
 * @brief Perform a Wi-Fi scan for the Web UI and return discovered APs sorted by RSSI
 */
esp_err_t wifi_manager_scan_aps(wifi_scan_item_t *out_items, size_t max_items, size_t *out_count);

#ifdef __cplusplus
}
#endif
