#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// for documentation: ssid needs to be a valid non-null null-terminated c-string <= 32 bytes
// for documentation: password may be null or a null-terminated valid c-string <= 63 bytes
// for documentation: out in wlink_sta_get_rssi must not be NULL
// for documentation: lifecycle: init -> connect -> (get_rssi) -> disconnect -> deinit, is_connected can be called whenever
// for documentation: nvs and event loop need to be initialized

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ssid;
    const char *password;
    uint32_t    connect_timeout_ms;
    int         max_retries;
} wlink_sta_settings_t;

#define WLINK_STA_DEFAULT_CONFIG() (wlink_sta_settings_t){ \
    .ssid               = CONFIG_WLINK_STA_SSID,           \
    .password           = CONFIG_WLINK_STA_PASSWORD,       \
    .connect_timeout_ms = 10000,                           \
    .max_retries        = 5,                               \
}

esp_err_t wlink_sta_init(const wlink_sta_settings_t *cfg);
esp_err_t wlink_sta_connect_start(void);
esp_err_t wlink_sta_wait_connected(void);
esp_err_t wlink_sta_disconnect(void);
esp_err_t wlink_sta_deinit(void);
bool wlink_sta_is_connected(void);
esp_err_t wlink_sta_get_rssi(int8_t *out);

#ifdef __cplusplus
}
#endif
