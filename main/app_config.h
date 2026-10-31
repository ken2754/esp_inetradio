#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_WIFI_APS        5
#define WIFI_SSID_MAX_LEN   32
#define WIFI_PASS_MAX_LEN   64

#define MAX_STATIONS        16
#define STATION_NAME_LEN    48
#define STATION_URL_LEN     192

#define DEFAULT_VOLUME      50

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASS_MAX_LEN + 1];
} wifi_ap_entry_t;

typedef struct {
    char name[STATION_NAME_LEN];
    char url[STATION_URL_LEN];
} radio_station_t;

typedef struct {
    wifi_ap_entry_t wifi_aps[MAX_WIFI_APS];
    radio_station_t stations[MAX_STATIONS];
    uint8_t station_count;
    uint8_t current_station;
    uint8_t volume; // 0 - 100
} app_config_t;

/**
 * @brief Initialize NVS and load configuration (or initialize defaults including SomaFM presets)
 */
esp_err_t app_config_init(void);

/**
 * @brief Copy current full configuration into out_cfg (thread-safe)
 */
void app_config_get(app_config_t *out_cfg);

/**
 * @brief Copy only Wi-Fi AP slots (avoids allocating full 4.3KB app_config_t on stack)
 */
void app_config_get_wifi_aps(wifi_ap_entry_t out_aps[MAX_WIFI_APS]);

/**
 * @brief Get lightweight station & volume metadata
 */
void app_config_get_station_meta(uint8_t *out_current, uint8_t *out_count, uint8_t *out_volume);

/**
 * @brief Update and persist Wi-Fi AP slots (up to MAX_WIFI_APS)
 */
esp_err_t app_config_set_wifi_aps(const wifi_ap_entry_t aps[MAX_WIFI_APS]);

/**
 * @brief Update and persist station list
 */
esp_err_t app_config_set_stations(const radio_station_t *stations, uint8_t count, int new_current_station);

/**
 * @brief Reset station list to default SomaFM presets and save to NVS
 */
esp_err_t app_config_reset_stations_to_somafm(void);

/**
 * @brief Set and persist volume (0 - 100)
 */
esp_err_t app_config_set_volume(uint8_t volume);

/**
 * @brief Get current volume (0 - 100)
 */
uint8_t app_config_get_volume(void);

/**
 * @brief Set and persist current station index
 */
esp_err_t app_config_set_current_station(uint8_t index);

/**
 * @brief Advance to the next station in loop (0 -> 1 -> ... -> count-1 -> 0) and persist
 * @param[out] out_station Optional pointer to receive the newly selected station info
 * @param[out] out_index   Optional pointer to receive the newly selected index
 */
esp_err_t app_config_next_station(radio_station_t *out_station, uint8_t *out_index);

/**
 * @brief Get the currently selected station
 */
bool app_config_get_current_station(radio_station_t *out_station, uint8_t *out_index);

#ifdef __cplusplus
}
#endif
