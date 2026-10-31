#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hardware Pin Definitions from HARDWARE.md
#define I2S_BCLK_GPIO       1
#define I2S_WS_GPIO         2
#define I2S_DOUT_GPIO       3

typedef enum {
    PLAYER_STATE_IDLE = 0,
    PLAYER_STATE_CONNECTING,
    PLAYER_STATE_BUFFERING,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_ERROR,
} player_state_t;

typedef struct {
    player_state_t state;
    char station_name[48];
    char station_url[192];
    char stream_title[128];
    char codec_name[16];
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t volume;
    uint32_t buffer_fill_pct;
} player_status_t;

/**
 * @brief Initialize I2S peripheral (PCM5102A), PSRAM ring buffer, and audio tasks
 */
esp_err_t audio_player_init(uint8_t initial_volume);

/**
 * @brief Start or switch playback to the specified station name and stream URL
 */
esp_err_t audio_player_play_station(const char *name, const char *url);

/**
 * @brief Stop current playback (e.g., when Wi-Fi disconnects)
 */
void audio_player_stop(void);

/**
 * @brief Set digital playback volume (0 - 100)
 */
void audio_player_set_volume(uint8_t volume);

/**
 * @brief Get current player status (thread-safe)
 */
void audio_player_get_status(player_status_t *out_status);

#ifdef __cplusplus
}
#endif
