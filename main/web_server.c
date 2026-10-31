#include "web_server.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "audio_player.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;

static esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return err;
}

static char *read_request_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > 8192) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    size_t received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        received += (size_t)r;
    }
    buf[len] = '\0';
    return buf;
}

static const char *player_state_to_str(player_state_t st)
{
    switch (st) {
        case PLAYER_STATE_IDLE:       return "IDLE";
        case PLAYER_STATE_CONNECTING: return "CONNECTING";
        case PLAYER_STATE_BUFFERING:  return "BUFFERING";
        case PLAYER_STATE_PLAYING:    return "PLAYING";
        case PLAYER_STATE_ERROR:      return "ERROR";
        default:                      return "UNKNOWN";
    }
}

/* GET / -> Serve embedded index.html */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    size_t html_len = (size_t)(index_html_end - index_html_start);
    if (html_len > 0 && index_html_start[html_len - 1] == '\0') {
        html_len--;
    }
    return httpd_resp_send(req, (const char *)index_html_start, html_len);
}

/* GET /api/status -> Current Wi-Fi, Player, and Config state */
static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    wifi_status_t wifi_st;
    player_status_t player_st;
    uint8_t current_station = 0;
    uint8_t station_count = 0;
    uint8_t volume = 50;

    wifi_manager_get_status(&wifi_st);
    audio_player_get_status(&player_st);
    app_config_get_station_meta(&current_station, &station_count, &volume);

    cJSON *root = cJSON_CreateObject();

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "sta_connected", wifi_st.sta_connected);
    cJSON_AddBoolToObject(wifi, "softap_active", wifi_st.softap_active);
    cJSON_AddStringToObject(wifi, "ssid", wifi_st.connected_ssid);
    cJSON_AddNumberToObject(wifi, "rssi", wifi_st.rssi);
    cJSON_AddStringToObject(wifi, "ip", wifi_st.ip_addr);

    cJSON *player = cJSON_AddObjectToObject(root, "player");
    cJSON_AddStringToObject(player, "state", player_state_to_str(player_st.state));
    cJSON_AddStringToObject(player, "station_name", player_st.station_name);
    cJSON_AddStringToObject(player, "station_url", player_st.station_url);
    cJSON_AddStringToObject(player, "stream_title", player_st.stream_title);
    cJSON_AddStringToObject(player, "codec", player_st.codec_name);
    cJSON_AddNumberToObject(player, "sample_rate", player_st.sample_rate);
    cJSON_AddNumberToObject(player, "channels", player_st.channels);
    cJSON_AddNumberToObject(player, "volume", volume);
    cJSON_AddNumberToObject(player, "buffer_pct", player_st.buffer_fill_pct);
    cJSON_AddNumberToObject(player, "current_station", current_station);
    cJSON_AddNumberToObject(player, "station_count", station_count);

    return send_json_response(req, root);
}

/* GET /api/config -> Full Wi-Fi AP slots (passwords masked) + Stations + Volume */
static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    app_config_t *cfg = calloc(1, sizeof(app_config_t));
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    app_config_get(cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "volume", cfg->volume);
    cJSON_AddNumberToObject(root, "current_station", cfg->current_station);

    cJSON *aps = cJSON_AddArrayToObject(root, "wifi_aps");
    for (int i = 0; i < MAX_WIFI_APS; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", cfg->wifi_aps[i].ssid);
        cJSON_AddBoolToObject(ap, "has_password", cfg->wifi_aps[i].password[0] != '\0');
        cJSON_AddItemToArray(aps, ap);
    }

    cJSON *stations = cJSON_AddArrayToObject(root, "stations");
    for (int i = 0; i < cfg->station_count; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "name", cfg->stations[i].name);
        cJSON_AddStringToObject(st, "url", cfg->stations[i].url);
        cJSON_AddItemToArray(stations, st);
    }

    free(cfg);
    return send_json_response(req, root);
}

/* POST /api/volume -> {"volume": 0..100} */
static esp_err_t api_volume_post_handler(httpd_req_t *req)
{
    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *vol_item = cJSON_GetObjectItem(json, "volume");
    if (!cJSON_IsNumber(vol_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing volume");
        return ESP_FAIL;
    }

    int vol = vol_item->valueint;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    cJSON_Delete(json);

    app_config_set_volume((uint8_t)vol);
    audio_player_set_volume((uint8_t)vol);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "volume", vol);
    return send_json_response(req, resp);
}

/* POST /api/station/select -> {"index": N} or {"next": true} */
static esp_err_t api_station_select_post_handler(httpd_req_t *req)
{
    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    radio_station_t st = {0};
    uint8_t selected_idx = 0;
    esp_err_t err = ESP_OK;

    cJSON *next_item = cJSON_GetObjectItem(json, "next");
    cJSON *idx_item  = cJSON_GetObjectItem(json, "index");

    if (cJSON_IsTrue(next_item)) {
        err = app_config_next_station(&st, &selected_idx);
    } else if (cJSON_IsNumber(idx_item) && idx_item->valueint >= 0) {
        err = app_config_set_current_station((uint8_t)idx_item->valueint);
        if (err == ESP_OK) {
            app_config_get_current_station(&st, &selected_idx);
        }
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(json);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid station selection");
        return ESP_FAIL;
    }

    wifi_status_t wifi_st;
    wifi_manager_get_status(&wifi_st);
    if (wifi_st.sta_connected) {
        audio_player_play_station(st.name, st.url);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "current_station", selected_idx);
    cJSON_AddStringToObject(resp, "name", st.name);
    cJSON_AddStringToObject(resp, "url", st.url);
    return send_json_response(req, resp);
}

/* POST /api/stations -> {"stations": [{"name":"...","url":"..."}, ...], "current_station": 0} */
static esp_err_t api_stations_post_handler(httpd_req_t *req)
{
    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *reset_item = cJSON_GetObjectItem(json, "reset_somafm");
    if (cJSON_IsTrue(reset_item)) {
        cJSON_Delete(json);
        app_config_reset_stations_to_somafm();
        radio_station_t st;
        uint8_t idx = 0;
        if (app_config_get_current_station(&st, &idx)) {
            wifi_status_t wifi_st;
            wifi_manager_get_status(&wifi_st);
            if (wifi_st.sta_connected) {
                audio_player_play_station(st.name, st.url);
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", true);
        return send_json_response(req, resp);
    }

    cJSON *arr = cJSON_GetObjectItem(json, "stations");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "stations array required");
        return ESP_FAIL;
    }

    radio_station_t *new_stations = calloc(MAX_STATIONS, sizeof(radio_station_t));
    if (!new_stations) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    uint8_t count = 0;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (count >= MAX_STATIONS) break;
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *url  = cJSON_GetObjectItem(item, "url");
        if (cJSON_IsString(name) && cJSON_IsString(url) &&
            name->valuestring[0] != '\0' && url->valuestring[0] != '\0') {
            strncpy(new_stations[count].name, name->valuestring, STATION_NAME_LEN - 1);
            strncpy(new_stations[count].url, url->valuestring, STATION_URL_LEN - 1);
            count++;
        }
    }

    int new_cur = -1;
    cJSON *cur_item = cJSON_GetObjectItem(json, "current_station");
    if (cJSON_IsNumber(cur_item)) {
        new_cur = cur_item->valueint;
    }
    cJSON_Delete(json);

    if (count == 0) {
        free(new_stations);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "At least one valid station is required");
        return ESP_FAIL;
    }

    app_config_set_stations(new_stations, count, new_cur);
    free(new_stations);

    radio_station_t st;
    uint8_t idx = 0;
    if (app_config_get_current_station(&st, &idx)) {
        wifi_status_t wifi_st;
        wifi_manager_get_status(&wifi_st);
        if (wifi_st.sta_connected) {
            audio_player_play_station(st.name, st.url);
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "station_count", count);
    return send_json_response(req, resp);
}

/* POST /api/wifi -> {"wifi_aps": [{"ssid":"...","password":"...","keep_password":true}, ...]} */
static esp_err_t api_wifi_post_handler(httpd_req_t *req)
{
    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_GetObjectItem(json, "wifi_aps");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wifi_aps array required");
        return ESP_FAIL;
    }

    wifi_ap_entry_t cur_aps[MAX_WIFI_APS];
    app_config_get_wifi_aps(cur_aps);

    wifi_ap_entry_t new_aps[MAX_WIFI_APS];
    memset(new_aps, 0, sizeof(new_aps));

    int idx = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (idx >= MAX_WIFI_APS) break;
        cJSON *ssid = cJSON_GetObjectItem(item, "ssid");
        cJSON *pass = cJSON_GetObjectItem(item, "password");
        cJSON *keep = cJSON_GetObjectItem(item, "keep_password");

        if (cJSON_IsString(ssid) && ssid->valuestring[0] != '\0') {
            strncpy(new_aps[idx].ssid, ssid->valuestring, WIFI_SSID_MAX_LEN);
            if (cJSON_IsTrue(keep) && strcmp(cur_aps[idx].ssid, new_aps[idx].ssid) == 0) {
                strncpy(new_aps[idx].password, cur_aps[idx].password, WIFI_PASS_MAX_LEN);
            } else if (cJSON_IsString(pass)) {
                strncpy(new_aps[idx].password, pass->valuestring, WIFI_PASS_MAX_LEN);
            }
        }
        idx++;
    }
    cJSON_Delete(json);

    app_config_set_wifi_aps(new_aps);
    wifi_manager_trigger_reconnect();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json_response(req, resp);
}

/* GET /api/wifi/scan -> Scan nearby Wi-Fi networks */
static esp_err_t api_wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_scan_item_t *items = calloc(MAX_SCAN_RESULTS, sizeof(wifi_scan_item_t));
    if (!items) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    esp_err_t err = wifi_manager_scan_aps(items, MAX_SCAN_RESULTS, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON *arr = cJSON_AddArrayToObject(root, "aps");
    if (err == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", items[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", items[i].rssi);
            cJSON_AddNumberToObject(ap, "authmode", items[i].authmode);
            cJSON_AddItemToArray(arr, ap);
        }
    }
    free(items);
    return send_json_response(req, root);
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 10240;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uri_index = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = index_get_handler,
    };
    static const httpd_uri_t uri_status = {
        .uri      = "/api/status",
        .method   = HTTP_GET,
        .handler  = api_status_get_handler,
    };
    static const httpd_uri_t uri_config = {
        .uri      = "/api/config",
        .method   = HTTP_GET,
        .handler  = api_config_get_handler,
    };
    static const httpd_uri_t uri_volume = {
        .uri      = "/api/volume",
        .method   = HTTP_POST,
        .handler  = api_volume_post_handler,
    };
    static const httpd_uri_t uri_station_select = {
        .uri      = "/api/station/select",
        .method   = HTTP_POST,
        .handler  = api_station_select_post_handler,
    };
    static const httpd_uri_t uri_stations = {
        .uri      = "/api/stations",
        .method   = HTTP_POST,
        .handler  = api_stations_post_handler,
    };
    static const httpd_uri_t uri_wifi = {
        .uri      = "/api/wifi",
        .method   = HTTP_POST,
        .handler  = api_wifi_post_handler,
    };
    static const httpd_uri_t uri_wifi_scan = {
        .uri      = "/api/wifi/scan",
        .method   = HTTP_GET,
        .handler  = api_wifi_scan_get_handler,
    };

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_config);
    httpd_register_uri_handler(s_server, &uri_volume);
    httpd_register_uri_handler(s_server, &uri_station_select);
    httpd_register_uri_handler(s_server, &uri_stations);
    httpd_register_uri_handler(s_server, &uri_wifi);
    httpd_register_uri_handler(s_server, &uri_wifi_scan);

    ESP_LOGI(TAG, "Web configuration server started on port %d", config.server_port);
    return ESP_OK;
}
