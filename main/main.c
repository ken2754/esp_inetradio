#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "mdns.h"

#include "app_config.h"
#include "wifi_manager.h"
#include "audio_player.h"
#include "web_server.h"

static const char *TAG = "main";

// From HARDWARE.md: GPIO 0 is onboard BOOT button (Active LOW, internal pull-up enabled)
#define BOOT_BUTTON_GPIO    0

static void on_boot_button_single_click(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;

    radio_station_t st = {0};
    uint8_t idx = 0;
    if (app_config_next_station(&st, &idx) == ESP_OK) {
        ESP_LOGI(TAG, "BOOT button pressed -> Selected station #%u: '%s' (%s)",
                 (unsigned)(idx + 1), st.name, st.url);

        wifi_status_t wifi_st;
        wifi_manager_get_status(&wifi_st);
        if (wifi_st.sta_connected) {
            audio_player_play_station(st.name, st.url);
        } else {
            ESP_LOGW(TAG, "Wi-Fi STA not connected yet; station selection saved for when connected");
        }
    }
}

static void init_boot_button(void)
{
    button_config_t btn_cfg = {
        .long_press_time = 1500,
        .short_press_time = 180,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BOOT_BUTTON_GPIO,
        .active_level = 0, // Active LOW
        .enable_power_save = false,
        .disable_pull = false, // Enable internal pull-up
    };

    button_handle_t btn_handle = NULL;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn_handle);
    if (err == ESP_OK && btn_handle != NULL) {
        iot_button_register_cb(btn_handle, BUTTON_SINGLE_CLICK, NULL, on_boot_button_single_click, NULL);
        ESP_LOGI(TAG, "Initialized BOOT button on GPIO %d (Active LOW)", BOOT_BUTTON_GPIO);
    } else {
        ESP_LOGE(TAG, "Failed to initialize BOOT button: %s", esp_err_to_name(err));
    }
}

static void init_mdns_service(void)
{
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("inetradio");
        mdns_instance_name_set("ESP32-S3 Internet Radio");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS started: http://inetradio.local");
    }
}

static void on_wifi_state_changed(bool sta_connected)
{
    if (sta_connected) {
        radio_station_t st = {0};
        uint8_t idx = 0;
        if (app_config_get_current_station(&st, &idx)) {
            ESP_LOGI(TAG, "Wi-Fi connected! Starting station #%u: '%s'", (unsigned)(idx + 1), st.name);
            audio_player_play_station(st.name, st.url);
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi STA disconnected, stopping audio player");
        audio_player_stop();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== SuperMini ESP32-S3 Internet Radio Starting ===");
    ESP_LOGI(TAG, "Free internal heap: %u bytes, Free PSRAM: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 1. Load configuration from NVS (or initialize SomaFM presets)
    ESP_ERROR_CHECK(app_config_init());

    // 2. Initialize Wi-Fi manager FIRST (before I2S GPIO 1,2,3 / mDNS / HTTP server, matching ESPresso)
    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_state_changed));

    // 3. Initialize I2S audio player (PCM5102A on GPIO 1, 2, 3) & PSRAM buffer
    ESP_ERROR_CHECK(audio_player_init(app_config_get_volume()));

    // Register state callback and start playback if initial Wi-Fi connection succeeded
    wifi_status_t wifi_st = {0};
    wifi_manager_get_status(&wifi_st);
    if (wifi_st.sta_connected) {
        on_wifi_state_changed(true);
    }

    // 4. Initialize BOOT button (GPIO 0) for looping through stations
    init_boot_button();

    // 5. Start mDNS and HTTP Web configuration server
    init_mdns_service();
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "System initialization complete.");
}
