#include "audio_player.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"

static const char *TAG = "audio_player";

#define PSRAM_RINGBUF_SIZE      (256 * 1024)
#define FALLBACK_RINGBUF_SIZE   (32 * 1024)
#define PREBUFFER_BYTES         (16 * 1024)

// Custom PSRAM-backed circular byte buffer with O(1) clear on station switch
typedef struct {
    uint8_t *buf;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    SemaphoreHandle_t mutex;
} audio_ringbuf_t;

static audio_ringbuf_t s_ringbuf;
static i2s_chan_handle_t s_tx_handle = NULL;
static uint32_t s_current_i2s_rate = 44100;
static bool s_i2s_enabled = false;

static SemaphoreHandle_t s_state_mutex = NULL;
static volatile uint32_t s_stream_gen = 0;
static volatile bool s_should_play = false;
static volatile uint8_t s_volume = 50;
static volatile esp_audio_simple_dec_type_t s_detected_codec = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;

static player_status_t s_player_status = {
    .state = PLAYER_STATE_IDLE,
    .station_name = "",
    .station_url = "",
    .stream_title = "",
    .codec_name = "MP3",
    .sample_rate = 44100,
    .channels = 2,
    .volume = 50,
    .buffer_fill_pct = 0
};

static void ringbuf_clear(void)
{
    xSemaphoreTake(s_ringbuf.mutex, portMAX_DELAY);
    s_ringbuf.head = 0;
    s_ringbuf.tail = 0;
    s_ringbuf.count = 0;
    xSemaphoreGive(s_ringbuf.mutex);
}

static size_t ringbuf_get_count(void)
{
    xSemaphoreTake(s_ringbuf.mutex, portMAX_DELAY);
    size_t c = s_ringbuf.count;
    xSemaphoreGive(s_ringbuf.mutex);
    return c;
}

static size_t ringbuf_write(const uint8_t *data, size_t len, uint32_t expected_gen)
{
    size_t written = 0;
    while (written < len) {
        if (s_stream_gen != expected_gen || !s_should_play) {
            break;
        }
        xSemaphoreTake(s_ringbuf.mutex, portMAX_DELAY);
        size_t space = s_ringbuf.capacity - s_ringbuf.count;
        if (space > 0) {
            size_t to_write = len - written;
            if (to_write > space) {
                to_write = space;
            }
            size_t first_chunk = s_ringbuf.capacity - s_ringbuf.head;
            if (to_write <= first_chunk) {
                memcpy(s_ringbuf.buf + s_ringbuf.head, data + written, to_write);
                s_ringbuf.head += to_write;
                if (s_ringbuf.head == s_ringbuf.capacity) {
                    s_ringbuf.head = 0;
                }
            } else {
                memcpy(s_ringbuf.buf + s_ringbuf.head, data + written, first_chunk);
                size_t second_chunk = to_write - first_chunk;
                memcpy(s_ringbuf.buf, data + written + first_chunk, second_chunk);
                s_ringbuf.head = second_chunk;
            }
            s_ringbuf.count += to_write;
            written += to_write;
            xSemaphoreGive(s_ringbuf.mutex);
        } else {
            xSemaphoreGive(s_ringbuf.mutex);
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    return written;
}

static size_t ringbuf_read(uint8_t *out, size_t max_len)
{
    xSemaphoreTake(s_ringbuf.mutex, portMAX_DELAY);
    if (s_ringbuf.count == 0) {
        xSemaphoreGive(s_ringbuf.mutex);
        return 0;
    }
    size_t to_read = (max_len < s_ringbuf.count) ? max_len : s_ringbuf.count;
    size_t first_chunk = s_ringbuf.capacity - s_ringbuf.tail;
    if (to_read <= first_chunk) {
        memcpy(out, s_ringbuf.buf + s_ringbuf.tail, to_read);
        s_ringbuf.tail += to_read;
        if (s_ringbuf.tail == s_ringbuf.capacity) {
            s_ringbuf.tail = 0;
        }
    } else {
        memcpy(out, s_ringbuf.buf + s_ringbuf.tail, first_chunk);
        size_t second_chunk = to_read - first_chunk;
        memcpy(out + first_chunk, s_ringbuf.buf, second_chunk);
        s_ringbuf.tail = second_chunk;
    }
    s_ringbuf.count -= to_read;
    xSemaphoreGive(s_ringbuf.mutex);
    return to_read;
}

static esp_err_t init_i2s_pcm5102a(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_current_i2s_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // PCM5102A uses internal PLL (SCK grounded/open)
            .bclk = GPIO_NUM_1,      // GPIO 1: BCLK
            .ws   = GPIO_NUM_2,      // GPIO 2: WS / LRCK
            .dout = GPIO_NUM_3,      // GPIO 3: DOUT / DIN
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));

    // Minimize GPIO drive strength on GPIO 1, 2, 3 (adjacent to SuperMini 2.4GHz antenna) to reduce RF harmonic EMI
    gpio_set_drive_capability(GPIO_NUM_1, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(GPIO_NUM_2, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(GPIO_NUM_3, GPIO_DRIVE_CAP_0);

    // Keep I2S disabled while idle so no 2.8MHz BCLK noise is radiated during Wi-Fi scanning
    s_i2s_enabled = false;
    ESP_LOGI(TAG, "I2S initialized for PCM5102A (BCLK=GPIO%d, WS=GPIO%d, DOUT=GPIO%d, idle-disabled)",
             I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO);
    return ESP_OK;
}

static void i2s_set_enabled(bool enable)
{
    if (!s_tx_handle) return;
    if (enable && !s_i2s_enabled) {
        if (i2s_channel_enable(s_tx_handle) == ESP_OK) {
            s_i2s_enabled = true;
        }
    } else if (!enable && s_i2s_enabled) {
        if (i2s_channel_disable(s_tx_handle) == ESP_OK) {
            s_i2s_enabled = false;
        }
    }
}

static void update_i2s_sample_rate(uint32_t sample_rate)
{
    if (sample_rate == 0 || sample_rate == s_current_i2s_rate) {
        return;
    }
    ESP_LOGI(TAG, "Reconfiguring I2S clock from %lu Hz to %lu Hz",
             (unsigned long)s_current_i2s_rate, (unsigned long)sample_rate);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    bool was_enabled = s_i2s_enabled;
    if (was_enabled) {
        i2s_set_enabled(false);
    }
    if (i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg) == ESP_OK) {
        s_current_i2s_rate = sample_rate;
    }
    if (was_enabled) {
        i2s_set_enabled(true);
    }
}

typedef struct {
    int metaint;
    char content_type[64];
} http_hdr_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_hdr_ctx_t *ctx = (http_hdr_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            ctx->metaint = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "content-type") == 0) {
            strncpy(ctx->content_type, evt->header_value, sizeof(ctx->content_type) - 1);
            ctx->content_type[sizeof(ctx->content_type) - 1] = '\0';
        }
    }
    return ESP_OK;
}

static void parse_icy_metadata(const char *meta_buf, size_t len)
{
    // Look for StreamTitle='...';
    const char *tag = strstr(meta_buf, "StreamTitle='");
    if (!tag) return;
    tag += strlen("StreamTitle='");
    const char *end = strstr(tag, "';");
    if (!end) {
        end = strchr(tag, '\'');
    }
    if (!end || end <= tag) return;

    size_t title_len = (size_t)(end - tag);
    if (title_len >= sizeof(s_player_status.stream_title)) {
        title_len = sizeof(s_player_status.stream_title) - 1;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memcpy(s_player_status.stream_title, tag, title_len);
    s_player_status.stream_title[title_len] = '\0';
    ESP_LOGI(TAG, "ICY StreamTitle: %s", s_player_status.stream_title);
    xSemaphoreGive(s_state_mutex);
}

static esp_audio_simple_dec_type_t detect_codec_from_info(const char *content_type, const char *url)
{
    if (content_type && content_type[0] != '\0') {
        if (strcasestr(content_type, "aac") != NULL || strcasestr(content_type, "mp4") != NULL) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        }
        if (strcasestr(content_type, "mpeg") != NULL || strcasestr(content_type, "mp3") != NULL) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        }
    }
    if (url) {
        if (strcasestr(url, "aac") != NULL) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        }
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

static void http_stream_task(void *arg)
{
    uint8_t *read_buf = malloc(2048);
    char *meta_buf = malloc(4096);

    while (1) {
        if (!s_should_play) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint32_t my_gen = s_stream_gen;
        char url[192] = {0};
        char station_name[48] = {0};

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        strncpy(url, s_player_status.station_url, sizeof(url) - 1);
        strncpy(station_name, s_player_status.station_name, sizeof(station_name) - 1);
        s_player_status.state = PLAYER_STATE_CONNECTING;
        s_player_status.stream_title[0] = '\0';
        xSemaphoreGive(s_state_mutex);

        if (url[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        ESP_LOGI(TAG, "Connecting to station '%s': %s", station_name, url);

        http_hdr_ctx_t hdr_ctx = {
            .metaint = 0,
            .content_type = ""
        };

        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .user_data = &hdr_ctx,
            .timeout_ms = 5000,
            .buffer_size = 4096,
            .buffer_size_tx = 1024,
            .max_redirection_count = 5,
            .keep_alive_enable = true,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_http_client_set_header(client, "User-Agent", "ESP32S3-InetRadio/1.0");
        esp_http_client_set_header(client, "Icy-MetaData", "1");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open stream connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (s_stream_gen == my_gen) {
                s_player_status.state = PLAYER_STATE_ERROR;
            }
            xSemaphoreGive(s_state_mutex);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int64_t content_len = esp_http_client_fetch_headers(client);
        (void)content_len;
        int status_code = esp_http_client_get_status_code(client);

        // Handle HTTP 301/302 redirect manually when using esp_http_client_open
        int redirects = 0;
        while ((status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) && redirects < 5) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            hdr_ctx.metaint = 0;
            hdr_ctx.content_type[0] = '\0';
            if (esp_http_client_open(client, 0) != ESP_OK) {
                status_code = -1;
                break;
            }
            esp_http_client_fetch_headers(client);
            status_code = esp_http_client_get_status_code(client);
            redirects++;
        }

        if (status_code != 200) {
            ESP_LOGE(TAG, "Unexpected HTTP status code: %d", status_code);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (s_stream_gen == my_gen) {
                s_player_status.state = PLAYER_STATE_ERROR;
            }
            xSemaphoreGive(s_state_mutex);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        esp_audio_simple_dec_type_t codec = detect_codec_from_info(hdr_ctx.content_type, url);
        s_detected_codec = codec;

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_stream_gen == my_gen) {
            s_player_status.state = PLAYER_STATE_BUFFERING;
            strncpy(s_player_status.codec_name,
                    (codec == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) ? "AAC" : "MP3",
                    sizeof(s_player_status.codec_name) - 1);
        }
        xSemaphoreGive(s_state_mutex);

        ESP_LOGI(TAG, "Stream opened: Content-Type='%s' (%s), icy-metaint=%d",
                 hdr_ctx.content_type,
                 (codec == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) ? "AAC" : "MP3",
                 hdr_ctx.metaint);

        int bytes_until_meta = hdr_ctx.metaint;

        while (s_should_play && s_stream_gen == my_gen) {
            int to_read = 2048;
            if (hdr_ctx.metaint > 0 && bytes_until_meta < to_read) {
                to_read = bytes_until_meta;
            }

            if (to_read > 0) {
                int rlen = esp_http_client_read(client, (char *)read_buf, to_read);
                if (rlen <= 0) {
                    ESP_LOGW(TAG, "Stream read ended or timed out (rlen=%d)", rlen);
                    break;
                }
                ringbuf_write(read_buf, (size_t)rlen, my_gen);
                if (hdr_ctx.metaint > 0) {
                    bytes_until_meta -= rlen;
                }
            }

            if (hdr_ctx.metaint > 0 && bytes_until_meta == 0 && s_stream_gen == my_gen) {
                uint8_t meta_len_byte = 0;
                int r = esp_http_client_read(client, (char *)&meta_len_byte, 1);
                if (r != 1) {
                    break;
                }
                int meta_bytes = (int)meta_len_byte * 16;
                if (meta_bytes > 0) {
                    int total_meta = 0;
                    while (total_meta < meta_bytes && s_stream_gen == my_gen) {
                        int mr = esp_http_client_read(client, meta_buf + total_meta, meta_bytes - total_meta);
                        if (mr <= 0) break;
                        total_meta += mr;
                    }
                    if (total_meta == meta_bytes) {
                        meta_buf[meta_bytes] = '\0';
                        parse_icy_metadata(meta_buf, (size_t)meta_bytes);
                    } else {
                        break;
                    }
                }
                bytes_until_meta = hdr_ctx.metaint;
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (s_should_play && s_stream_gen == my_gen) {
            ESP_LOGW(TAG, "Stream interrupted, reconnecting in 1s...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void apply_volume_and_write_i2s(const int16_t *pcm_in, size_t sample_count, uint8_t channels, int16_t *stereo_out)
{
    // Quadratic volume curve (0..100 -> 0..10000) for natural human loudness perception
    uint32_t vol = s_volume;
    uint32_t scale = (vol * vol); // 0 .. 10000

    if (channels == 1) {
        // Duplicate mono sample to Left and Right stereo slots
        size_t frames = sample_count;
        size_t idx = 0;
        while (idx < frames) {
            size_t chunk = (frames - idx > 1024) ? 1024 : (frames - idx);
            for (size_t i = 0; i < chunk; i++) {
                int32_t s = ((int32_t)pcm_in[idx + i] * (int32_t)scale) / 10000;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                stereo_out[2 * i]     = (int16_t)s;
                stereo_out[2 * i + 1] = (int16_t)s;
            }
            size_t bytes_written = 0;
            i2s_channel_write(s_tx_handle, stereo_out, chunk * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(200));
            idx += chunk;
        }
    } else {
        // Stereo samples (2 channels)
        size_t idx = 0;
        while (idx < sample_count) {
            size_t chunk = (sample_count - idx > 2048) ? 2048 : (sample_count - idx);
            for (size_t i = 0; i < chunk; i++) {
                int32_t s = ((int32_t)pcm_in[idx + i] * (int32_t)scale) / 10000;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                stereo_out[i] = (int16_t)s;
            }
            size_t bytes_written = 0;
            i2s_channel_write(s_tx_handle, stereo_out, chunk * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(200));
            idx += chunk;
        }
    }
}

static void audio_decoder_task(void *arg)
{
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    size_t in_cap = 2048;
    size_t out_cap = 8192;
    uint8_t *in_buf = malloc(in_cap);
    uint8_t *out_buf = malloc(out_cap);
    int16_t *stereo_buf = malloc(2048 * sizeof(int16_t));

    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_simple_dec_type_t open_codec = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    uint32_t active_gen = 0;
    size_t in_fill = 0;
    bool prebuffered = false;

    while (1) {
        if (!s_should_play) {
            i2s_set_enabled(false);
            if (decoder) {
                esp_audio_simple_dec_close(decoder);
                decoder = NULL;
                open_codec = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
            }
            in_fill = 0;
            prebuffered = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (active_gen != s_stream_gen) {
            active_gen = s_stream_gen;
            in_fill = 0;
            prebuffered = false;
            if (decoder) {
                esp_audio_simple_dec_close(decoder);
                decoder = NULL;
                open_codec = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
            }
        }

        size_t buffered = ringbuf_get_count();
        if (!prebuffered) {
            if (buffered < PREBUFFER_BYTES) {
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;
            }
            prebuffered = true;
            i2s_set_enabled(true);
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (s_stream_gen == active_gen) {
                s_player_status.state = PLAYER_STATE_PLAYING;
            }
            xSemaphoreGive(s_state_mutex);
        }

        // Open or reopen decoder if codec changed
        esp_audio_simple_dec_type_t target_codec = s_detected_codec;
        if (decoder == NULL || open_codec != target_codec) {
            if (decoder) {
                esp_audio_simple_dec_close(decoder);
                decoder = NULL;
            }
            esp_aac_dec_cfg_t aac_cfg = {
                .aac_plus_enable = true,
            };
            esp_audio_simple_dec_cfg_t dec_cfg = {
                .dec_type = target_codec,
                .dec_cfg = (target_codec == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) ? &aac_cfg : NULL,
                .cfg_size = (target_codec == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) ? sizeof(aac_cfg) : 0,
                .use_frame_dec = false,
            };
            if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Failed to open simple decoder for type %d", target_codec);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            open_codec = target_codec;
        }

        // Top up input buffer from ring buffer
        if (in_fill < in_cap) {
            size_t got = ringbuf_read(in_buf + in_fill, in_cap - in_fill);
            in_fill += got;
        }

        if (in_fill == 0) {
            // Buffer underrun: wait for prebuffer again
            prebuffered = false;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (s_stream_gen == active_gen && s_player_status.state == PLAYER_STATE_PLAYING) {
                s_player_status.state = PLAYER_STATE_BUFFERING;
            }
            xSemaphoreGive(s_state_mutex);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = in_buf,
            .len = (uint32_t)in_fill,
            .eos = false,
            .consumed = 0,
        };
        esp_audio_simple_dec_out_t out_frame = {
            .buffer = out_buf,
            .len = (uint32_t)out_cap,
            .needed_size = 0,
            .decoded_size = 0,
        };

        esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out_frame.needed_size > out_cap) {
            uint8_t *new_out = realloc(out_buf, out_frame.needed_size);
            if (new_out) {
                out_buf = new_out;
                out_cap = out_frame.needed_size;
            }
        }

        if (raw.consumed > 0 && raw.consumed <= in_fill) {
            size_t rem = in_fill - raw.consumed;
            if (rem > 0) {
                memmove(in_buf, in_buf + raw.consumed, rem);
            }
            in_fill = rem;
        } else if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            // Skip 1 byte to resync on corrupted stream data
            if (in_fill > 0) {
                memmove(in_buf, in_buf + 1, in_fill - 1);
                in_fill--;
            }
        }

        if (out_frame.decoded_size > 0 && s_stream_gen == active_gen) {
            esp_audio_simple_dec_info_t dec_info = {0};
            if (esp_audio_simple_dec_get_info(decoder, &dec_info) == ESP_AUDIO_ERR_OK && dec_info.sample_rate > 0) {
                update_i2s_sample_rate(dec_info.sample_rate);
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_player_status.sample_rate = dec_info.sample_rate;
                s_player_status.channels = dec_info.channel;
                xSemaphoreGive(s_state_mutex);
            }

            uint8_t ch = (dec_info.channel > 0) ? dec_info.channel : 2;
            size_t sample_count = out_frame.decoded_size / sizeof(int16_t);
            apply_volume_and_write_i2s((const int16_t *)out_frame.buffer, sample_count, ch, stereo_buf);
        }
    }
}

esp_err_t audio_player_init(uint8_t initial_volume)
{
    s_state_mutex = xSemaphoreCreateMutex();
    s_ringbuf.mutex = xSemaphoreCreateMutex();
    s_volume = (initial_volume <= 100) ? initial_volume : 50;
    s_player_status.volume = s_volume;

    // Allocate 256KB ring buffer in PSRAM (or 32KB in internal RAM if PSRAM unavailable)
    s_ringbuf.buf = heap_caps_malloc(PSRAM_RINGBUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ringbuf.buf) {
        s_ringbuf.capacity = PSRAM_RINGBUF_SIZE;
        ESP_LOGI(TAG, "Allocated %u KB stream ring buffer in PSRAM", (unsigned)(PSRAM_RINGBUF_SIZE / 1024));
    } else {
        s_ringbuf.buf = heap_caps_malloc(FALLBACK_RINGBUF_SIZE, MALLOC_CAP_8BIT);
        s_ringbuf.capacity = FALLBACK_RINGBUF_SIZE;
        ESP_LOGW(TAG, "PSRAM unavailable; allocated %u KB stream ring buffer in internal RAM",
                 (unsigned)(FALLBACK_RINGBUF_SIZE / 1024));
    }
    if (!s_ringbuf.buf) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(init_i2s_pcm5102a());

    xTaskCreatePinnedToCore(http_stream_task, "http_stream", 8192, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(audio_decoder_task, "audio_dec", 16384, NULL, 7, NULL, 1);

    return ESP_OK;
}

esp_err_t audio_player_play_station(const char *name, const char *url)
{
    if (!name || !url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strncpy(s_player_status.station_name, name, sizeof(s_player_status.station_name) - 1);
    s_player_status.station_name[sizeof(s_player_status.station_name) - 1] = '\0';
    strncpy(s_player_status.station_url, url, sizeof(s_player_status.station_url) - 1);
    s_player_status.station_url[sizeof(s_player_status.station_url) - 1] = '\0';
    s_player_status.stream_title[0] = '\0';
    s_player_status.state = PLAYER_STATE_CONNECTING;
    s_stream_gen++;
    s_should_play = true;
    xSemaphoreGive(s_state_mutex);

    ringbuf_clear();
    ESP_LOGI(TAG, "Switching station -> '%s' (%s)", name, url);
    return ESP_OK;
}

void audio_player_stop(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_should_play = false;
    s_stream_gen++;
    s_player_status.state = PLAYER_STATE_IDLE;
    s_player_status.stream_title[0] = '\0';
    xSemaphoreGive(s_state_mutex);

    ringbuf_clear();
}

void audio_player_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_volume = volume;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_player_status.volume = volume;
    xSemaphoreGive(s_state_mutex);
}

void audio_player_get_status(player_status_t *out_status)
{
    if (!out_status) return;
    size_t buffered = ringbuf_get_count();
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out_status = s_player_status;
    out_status->volume = s_volume;
    if (s_ringbuf.capacity > 0) {
        out_status->buffer_fill_pct = (uint32_t)((buffered * 100ULL) / s_ringbuf.capacity);
    } else {
        out_status->buffer_fill_pct = 0;
    }
    xSemaphoreGive(s_state_mutex);
}
