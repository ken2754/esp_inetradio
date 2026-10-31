#include "app_config.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "app_config";
static const char *NVS_NAMESPACE = "inetradio";

static app_config_t s_config;
static SemaphoreHandle_t s_cfg_mutex = NULL;

static const radio_station_t s_somafm_presets[] = {
    {
        .name = "SomaFM: Groove Salad",
        .url  = "http://ice1.somafm.com/groovesalad-128-mp3"
    },
    {
        .name = "SomaFM: Drone Zone",
        .url  = "http://ice1.somafm.com/dronezone-128-mp3"
    },
    {
        .name = "SomaFM: Secret Agent",
        .url  = "http://ice1.somafm.com/secretagent-128-mp3"
    },
    {
        .name = "SomaFM: DEF CON Radio",
        .url  = "http://ice1.somafm.com/defcon-128-mp3"
    },
    {
        .name = "SomaFM: Lush",
        .url  = "http://ice1.somafm.com/lush-128-mp3"
    },
    {
        .name = "SomaFM: Space Station",
        .url  = "http://ice1.somafm.com/spacestation-128-mp3"
    },
};

#define SOMAFM_PRESET_COUNT (sizeof(s_somafm_presets) / sizeof(s_somafm_presets[0]))

static void load_default_stations_locked(void)
{
    memset(s_config.stations, 0, sizeof(s_config.stations));
    s_config.station_count = SOMAFM_PRESET_COUNT;
    for (size_t i = 0; i < SOMAFM_PRESET_COUNT; i++) {
        s_config.stations[i] = s_somafm_presets[i];
    }
    s_config.current_station = 0;
}

static esp_err_t save_to_nvs_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, "config_v1", &s_config, sizeof(app_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config to NVS: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_config_init(void)
{
    if (s_cfg_mutex == NULL) {
        s_cfg_mutex = xSemaphoreCreateMutex();
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to recover");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);

    nvs_handle_t handle;
    bool loaded = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t required_size = sizeof(app_config_t);
        if (nvs_get_blob(handle, "config_v1", &s_config, &required_size) == ESP_OK &&
            required_size == sizeof(app_config_t)) {
            loaded = true;
        }
        nvs_close(handle);
    }

    if (!loaded) {
        ESP_LOGI(TAG, "Initializing default configuration with SomaFM presets");
        memset(&s_config, 0, sizeof(s_config));
        s_config.volume = DEFAULT_VOLUME;
        load_default_stations_locked();
        save_to_nvs_locked();
    } else {
        // Sanitize loaded values
        if (s_config.volume > 100) {
            s_config.volume = DEFAULT_VOLUME;
        }
        if (s_config.station_count == 0 || s_config.station_count > MAX_STATIONS) {
            load_default_stations_locked();
        }
        if (s_config.current_station >= s_config.station_count) {
            s_config.current_station = 0;
        }
        for (int i = 0; i < MAX_WIFI_APS; i++) {
            s_config.wifi_aps[i].ssid[WIFI_SSID_MAX_LEN] = '\0';
            s_config.wifi_aps[i].password[WIFI_PASS_MAX_LEN] = '\0';
        }
        for (int i = 0; i < MAX_STATIONS; i++) {
            s_config.stations[i].name[STATION_NAME_LEN - 1] = '\0';
            s_config.stations[i].url[STATION_URL_LEN - 1] = '\0';
        }
        ESP_LOGI(TAG, "Loaded config: %u stations, current=%u (%s), volume=%u%%",
                 s_config.station_count, s_config.current_station,
                 s_config.stations[s_config.current_station].name, s_config.volume);
    }

    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

void app_config_get(app_config_t *out_cfg)
{
    if (!out_cfg) return;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out_cfg = s_config;
    xSemaphoreGive(s_cfg_mutex);
}

void app_config_get_wifi_aps(wifi_ap_entry_t out_aps[MAX_WIFI_APS])
{
    if (!out_aps) return;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(out_aps, s_config.wifi_aps, sizeof(s_config.wifi_aps));
    xSemaphoreGive(s_cfg_mutex);
}

void app_config_get_station_meta(uint8_t *out_current, uint8_t *out_count, uint8_t *out_volume)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (out_current) *out_current = s_config.current_station;
    if (out_count)   *out_count   = s_config.station_count;
    if (out_volume)  *out_volume  = s_config.volume;
    xSemaphoreGive(s_cfg_mutex);
}

esp_err_t app_config_set_wifi_aps(const wifi_ap_entry_t aps[MAX_WIFI_APS])
{
    if (!aps) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WIFI_APS; i++) {
        memset(&s_config.wifi_aps[i], 0, sizeof(wifi_ap_entry_t));
        strncpy(s_config.wifi_aps[i].ssid, aps[i].ssid, WIFI_SSID_MAX_LEN);
        strncpy(s_config.wifi_aps[i].password, aps[i].password, WIFI_PASS_MAX_LEN);
    }
    esp_err_t err = save_to_nvs_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_set_stations(const radio_station_t *stations, uint8_t count, int new_current_station)
{
    if (!stations || count == 0 || count > MAX_STATIONS) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memset(s_config.stations, 0, sizeof(s_config.stations));
    s_config.station_count = count;
    for (uint8_t i = 0; i < count; i++) {
        strncpy(s_config.stations[i].name, stations[i].name, STATION_NAME_LEN - 1);
        strncpy(s_config.stations[i].url, stations[i].url, STATION_URL_LEN - 1);
    }
    if (new_current_station >= 0 && new_current_station < count) {
        s_config.current_station = (uint8_t)new_current_station;
    } else if (s_config.current_station >= count) {
        s_config.current_station = 0;
    }
    esp_err_t err = save_to_nvs_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_reset_stations_to_somafm(void)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    load_default_stations_locked();
    esp_err_t err = save_to_nvs_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_config.volume = volume;
    esp_err_t err = save_to_nvs_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

uint8_t app_config_get_volume(void)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    uint8_t vol = s_config.volume;
    xSemaphoreGive(s_cfg_mutex);
    return vol;
}

esp_err_t app_config_set_current_station(uint8_t index)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (index >= s_config.station_count) {
        xSemaphoreGive(s_cfg_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    s_config.current_station = index;
    esp_err_t err = save_to_nvs_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_next_station(radio_station_t *out_station, uint8_t *out_index)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (s_config.station_count == 0) {
        xSemaphoreGive(s_cfg_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_config.current_station = (s_config.current_station + 1) % s_config.station_count;
    if (out_station) {
        *out_station = s_config.stations[s_config.current_station];
    }
    if (out_index) {
        *out_index = s_config.current_station;
    }
    esp_err_t err = save_to_nvs_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

bool app_config_get_current_station(radio_station_t *out_station, uint8_t *out_index)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (s_config.station_count == 0) {
        xSemaphoreGive(s_cfg_mutex);
        return false;
    }
    uint8_t idx = s_config.current_station;
    if (idx >= s_config.station_count) {
        idx = 0;
        s_config.current_station = 0;
    }
    if (out_station) {
        *out_station = s_config.stations[idx];
    }
    if (out_index) {
        *out_index = idx;
    }
    xSemaphoreGive(s_cfg_mutex);
    return true;
}
