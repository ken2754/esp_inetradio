#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTP configuration and control web server on port 80
 */
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif
