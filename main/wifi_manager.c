#include "wifi_manager.h"
#include "app_config.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_phy_init.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "esp_log.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define WIFI_RECONNECT_REQ      BIT2
#define WIFI_STA_STARTED_BIT    BIT3

#define CONNECT_RETRIES_PER_AP  2
#define CONNECT_TIMEOUT_MS      12000

static EventGroupHandle_t s_wifi_event_group = NULL;
static SemaphoreHandle_t s_wifi_mutex = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static wifi_state_cb_t s_state_cb = NULL;

static wifi_status_t s_status = {
    .sta_connected = false,
    .softap_active = false,
    .connected_ssid = "",
    .rssi = 0,
    .ip_addr = "0.0.0.0"
};

static volatile bool s_connecting_attempt = false;
static int s_retry_num = 0;

typedef struct {
    int config_idx;
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASS_MAX_LEN + 1];
    int8_t rssi;
} candidate_ap_t;

static int cmp_candidate_rssi_desc(const void *a, const void *b)
{
    const candidate_ap_t *ca = (const candidate_ap_t *)a;
    const candidate_ap_t *cb = (const candidate_ap_t *)b;
    return (int)cb->rssi - (int)ca->rssi;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STA_STARTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = dis ? dis->reason : 0;

        bool was_connected = false;
        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
        was_connected = s_status.sta_connected;
        s_status.sta_connected = false;
        s_status.rssi = 0;
        xSemaphoreGive(s_wifi_mutex);

        if (s_connecting_attempt) {
            if (s_retry_num < CONNECT_RETRIES_PER_AP) {
                s_retry_num++;
                ESP_LOGI(TAG, "STA disconnected (reason=%u), retrying (%d/%d)...",
                         reason, s_retry_num, CONNECT_RETRIES_PER_AP);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "STA connection attempt failed (reason=%u)", reason);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (was_connected) {
            ESP_LOGW(TAG, "Lost Wi-Fi connection (reason=%u), scheduling rescan & reconnect", reason);
            if (s_state_cb) {
                s_state_cb(false);
            }
            xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_REQ);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "Connected! Got IP: %s", ip_str);

        wifi_ap_record_t ap_info;
        int8_t rssi = 0;
        char ssid[33] = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
            strncpy(ssid, (const char *)ap_info.ssid, sizeof(ssid) - 1);
        }

        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
        s_status.sta_connected = true;
        s_status.softap_active = false;
        strncpy(s_status.ip_addr, ip_str, sizeof(s_status.ip_addr) - 1);
        if (ssid[0] != '\0') {
            strncpy(s_status.connected_ssid, ssid, sizeof(s_status.connected_ssid) - 1);
        }
        s_status.rssi = rssi;
        xSemaphoreGive(s_wifi_mutex);

        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_softap_fallback(void)
{
    ESP_LOGW(TAG, "Starting SoftAP '%s' at %s", SOFTAP_SSID, SOFTAP_IP_ADDR);

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    // Configure static IP 192.168.4.1 on AP netif
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    esp_netif_dhcps_start(s_ap_netif);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = SOFTAP_SSID,
            .ssid_len = strlen(SOFTAP_SSID),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    // Use APSTA mode so the user can still perform Wi-Fi scans from the Web UI while connected to SoftAP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    s_status.sta_connected = false;
    s_status.softap_active = true;
    strncpy(s_status.connected_ssid, SOFTAP_SSID, sizeof(s_status.connected_ssid) - 1);
    s_status.rssi = 0;
    strncpy(s_status.ip_addr, SOFTAP_IP_ADDR, sizeof(s_status.ip_addr) - 1);
    xSemaphoreGive(s_wifi_mutex);

    if (s_state_cb) {
        s_state_cb(false);
    }
}

static bool scan_and_connect_strongest_ap(void)
{
    wifi_ap_entry_t wifi_aps[MAX_WIFI_APS];
    app_config_get_wifi_aps(wifi_aps);

    int configured_count = 0;
    for (int i = 0; i < MAX_WIFI_APS; i++) {
        if (wifi_aps[i].ssid[0] != '\0') {
            ESP_LOGI(TAG, "Registered AP slot #%d: SSID='%s'", i + 1, wifi_aps[i].ssid);
            configured_count++;
        }
    }

    if (configured_count == 0) {
        ESP_LOGI(TAG, "No Wi-Fi APs configured in NVS");
        return false;
    }

    bool is_conn = false;
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    is_conn = s_status.sta_connected;
    xSemaphoreGive(s_wifi_mutex);
    if (is_conn) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_err));
        return false;
    }
    xEventGroupWaitBits(s_wifi_event_group, WIFI_STA_STARTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));

    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(34); // 8.5 dBm

    uint16_t ap_num = 0;
    for (int scan_try = 0; scan_try < 3 && ap_num == 0; scan_try++) {
        if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
            esp_wifi_scan_get_ap_num(&ap_num);
            ESP_LOGI(TAG, "Wi-Fi scan (try %d/3) completed: %u APs detected", scan_try + 1, ap_num);
        }
    }

    if (ap_num == 0) {
        ESP_LOGW(TAG, "No access points found during scan -> starting SoftAP immediately");
        return false;
    }

    uint16_t max_records = (ap_num > 32) ? 32 : ap_num;
    wifi_ap_record_t *ap_records = calloc(max_records, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        return false;
    }

    candidate_ap_t *candidates = calloc(MAX_WIFI_APS, sizeof(candidate_ap_t));
    if (!candidates) {
        free(ap_records);
        return false;
    }
    int candidate_count = 0;

    if (esp_wifi_scan_get_ap_records(&max_records, ap_records) == ESP_OK) {
        for (uint16_t j = 0; j < max_records && j < 10; j++) {
            ESP_LOGI(TAG, "  Scanned AP[%u]: SSID='%s' RSSI=%d dBm ch=%u",
                     j, (const char *)ap_records[j].ssid, ap_records[j].rssi, ap_records[j].primary);
        }

        for (int i = 0; i < MAX_WIFI_APS; i++) {
            if (wifi_aps[i].ssid[0] == '\0') {
                continue;
            }
            int8_t best_rssi = -127;
            bool found = false;
            for (uint16_t j = 0; j < max_records; j++) {
                if (strcmp((const char *)ap_records[j].ssid, wifi_aps[i].ssid) == 0) {
                    if (!found || ap_records[j].rssi > best_rssi) {
                        best_rssi = ap_records[j].rssi;
                        found = true;
                    }
                }
            }
            if (found) {
                candidates[candidate_count].config_idx = i;
                strncpy(candidates[candidate_count].ssid, wifi_aps[i].ssid, WIFI_SSID_MAX_LEN);
                candidates[candidate_count].ssid[WIFI_SSID_MAX_LEN] = '\0';
                strncpy(candidates[candidate_count].password, wifi_aps[i].password, WIFI_PASS_MAX_LEN);
                candidates[candidate_count].password[WIFI_PASS_MAX_LEN] = '\0';
                candidates[candidate_count].rssi = best_rssi;
                candidate_count++;
                ESP_LOGI(TAG, "Matched registered AP [#%d]: SSID='%s' RSSI=%d dBm",
                         i + 1, wifi_aps[i].ssid, best_rssi);
            }
        }
    }
    free(ap_records);

    if (candidate_count == 0) {
        ESP_LOGW(TAG, "No registered APs found in scan results -> starting SoftAP immediately");
        free(candidates);
        return false;
    }

    if (candidate_count > 1) {
        // Sort matched candidates by strongest RSSI first
        qsort(candidates, candidate_count, sizeof(candidate_ap_t), cmp_candidate_rssi_desc);
    }

    // Try connecting in order of strongest RSSI
    for (int i = 0; i < candidate_count; i++) {
        ESP_LOGI(TAG, "Attempting connection to strongest AP #%d/%d: '%s' (RSSI %d dBm)",
                 i + 1, candidate_count, candidates[i].ssid, candidates[i].rssi);

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid, candidates[i].ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, candidates[i].password, sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        if (candidates[i].password[0] == '\0') {
            wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        s_retry_num = 0;
        s_connecting_attempt = true;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)
        );
        s_connecting_attempt = false;

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Successfully connected to '%s'", candidates[i].ssid);
            free(candidates);
            if (s_state_cb) {
                s_state_cb(true);
            }
            return true;
        }

        ESP_LOGW(TAG, "Failed to connect to '%s', trying next candidate if available", candidates[i].ssid);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    free(candidates);
    return false;
}

static void wifi_manager_task(void *arg)
{
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_RECONNECT_REQ,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(10000)
        );

        if (bits & WIFI_RECONNECT_REQ) {
            // Brief delay to allow HTTP responses to flush before changing Wi-Fi mode
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP_LOGI(TAG, "Executing requested Wi-Fi rescan and connection...");
            if (!scan_and_connect_strongest_ap()) {
                start_softap_fallback();
            }
        } else {
            bool is_conn = false;
            bool is_ap = false;
            xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
            is_conn = s_status.sta_connected;
            is_ap = s_status.softap_active;
            xSemaphoreGive(s_wifi_mutex);

            if (!is_conn && is_ap) {
                wifi_sta_list_t sta_list = {0};
                if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num == 0) {
                    ESP_LOGI(TAG, "SoftAP idle (0 clients): performing background Wi-Fi rescan...");
                    if (!scan_and_connect_strongest_ap()) {
                        start_softap_fallback();
                    }
                }
            }
        }
    }
}

esp_err_t wifi_manager_init(wifi_state_cb_t state_cb)
{
    s_wifi_event_group = xEventGroupCreate();
    s_wifi_mutex = xSemaphoreCreateMutex();

    // Clear any stale internal Wi-Fi NVS state
    nvs_handle_t wifi_nvs;
    if (nvs_open("nvs.net80211", NVS_READWRITE, &wifi_nvs) == ESP_OK) {
        nvs_erase_all(wifi_nvs);
        nvs_commit(wifi_nvs);
        nvs_close(wifi_nvs);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Perform initial scan & connection synchronously (same as ESPresso's init_wifi)
    if (!scan_and_connect_strongest_ap()) {
        start_softap_fallback();
    }

    s_state_cb = state_cb;
    xTaskCreate(wifi_manager_task, "wifi_mgr_task", 8192, NULL, 5, NULL);
    return ESP_OK;
}

void wifi_manager_trigger_reconnect(void)
{
    if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_REQ);
    }
}

void wifi_manager_get_status(wifi_status_t *out_status)
{
    if (!out_status) return;
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    *out_status = s_status;
    xSemaphoreGive(s_wifi_mutex);

    if (out_status->sta_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            out_status->rssi = ap_info.rssi;
        }
    }
}

esp_err_t wifi_manager_scan_aps(wifi_scan_item_t *out_items, size_t max_items, size_t *out_count)
{
    if (!out_items || !out_count || max_items == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    uint16_t ap_num = 0;
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&ap_num);
    }

    if (err != ESP_OK || ap_num == 0) {
        return err;
    }

    uint16_t fetch_num = (ap_num > 32) ? 32 : ap_num;
    wifi_ap_record_t *records = calloc(fetch_num, sizeof(wifi_ap_record_t));
    if (!records) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&fetch_num, records);
    if (err == ESP_OK) {
        size_t count = 0;
        for (uint16_t i = 0; i < fetch_num && count < max_items; i++) {
            if (records[i].ssid[0] == '\0') continue;
            // Deduplicate SSIDs (keep strongest RSSI)
            bool dup = false;
            for (size_t k = 0; k < count; k++) {
                if (strcmp(out_items[k].ssid, (const char *)records[i].ssid) == 0) {
                    if (records[i].rssi > out_items[k].rssi) {
                        out_items[k].rssi = records[i].rssi;
                    }
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                strncpy(out_items[count].ssid, (const char *)records[i].ssid, 32);
                out_items[count].ssid[32] = '\0';
                out_items[count].rssi = records[i].rssi;
                out_items[count].authmode = records[i].authmode;
                count++;
            }
        }
        *out_count = count;
    }

    free(records);
    return err;
}
