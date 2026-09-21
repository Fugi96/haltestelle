#include "wlink_sta.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_types_generic.h"
#include "freertos/idf_additions.h"
#include "esp_check.h"
#include "freertos/projdefs.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "wlink_sta";

#define WIFI_CONNECTED_BIT         BIT0
#define WIFI_FAIL_BIT              BIT1
#define WIFI_DISCONNECTED_BIT      BIT2
#define WLINK_STA_SSID_MAX_LEN     32
#define WLINK_STA_PASSWORD_MAX_LEN 63

static EventGroupHandle_t            s_wifi_events;
static esp_netif_t                  *s_netif;
static esp_event_handler_instance_t  s_wifi_evt_inst;
static esp_event_handler_instance_t  s_ip_evt_inst;
static int                           s_max_retries;
static int                           s_retry_num;
static uint32_t                      s_connect_timeout_ms;
static bool                          s_user_disconnect;

static void wlink_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGD(TAG, "sta started, associating...");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_user_disconnect) {
            xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
            return;
        }
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_retry_num < s_max_retries) {
            s_retry_num++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d",
                     d->reason, s_retry_num, s_max_retries);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        } else {
            ESP_LOGE(TAG, "connect failed after %d retries (reason %d)",
                     s_max_retries, d->reason);
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ip = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ip->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wlink_sta_init(const wlink_sta_settings_t *cfg) {
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(cfg && cfg->ssid, ESP_ERR_INVALID_ARG, TAG, "null cfg/ssid");
    ESP_RETURN_ON_FALSE(strlen(cfg->ssid) <= WLINK_STA_SSID_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "ssid too long, max 32 bytes");
    ESP_RETURN_ON_FALSE(!cfg->password || strlen(cfg->password) <= WLINK_STA_PASSWORD_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "password too long, max 63 bytes");

    s_max_retries = cfg->max_retries;

    s_connect_timeout_ms = cfg->connect_timeout_ms;

    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events, ESP_ERR_NO_MEM, TAG, "event group alloc");

    ESP_GOTO_ON_ERROR(esp_netif_init(), err_delete_event_group, TAG, "netif_init");

    s_netif = esp_netif_create_default_wifi_sta();
    ESP_GOTO_ON_FALSE(s_netif, ESP_FAIL, err_delete_event_group, TAG, "create sta netif");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_GOTO_ON_ERROR(esp_wifi_init(&init_cfg), err_destroy_netif, TAG, "wifi_init (NVS not initialized? call nvs_flash_init)");

    ESP_GOTO_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
            &wlink_event_handler, NULL, &s_wifi_evt_inst),
        err_deinit_wifi, TAG, "reg wifi handler (default event loop not created? call esp_event_loop_create_default)");

    ESP_GOTO_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            &wlink_event_handler, NULL, &s_ip_evt_inst),
        err_unregister_wifi_handler, TAG, "reg ip handler (default event loop not created? call esp_event_loop_create_default)");

    wifi_config_t wifi_config = { .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    strncpy((char *)wifi_config.sta.ssid, cfg->ssid, sizeof(wifi_config.sta.ssid));
    if (cfg->password) {
        strncpy((char *)wifi_config.sta.password, cfg->password, sizeof(wifi_config.sta.password));
    }

    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), err_unregister_ip_handler, TAG, "set_mode");
    ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), err_unregister_ip_handler, TAG, "set_config");

    ESP_LOGD(TAG, "initialized for SSID \"%s\"", cfg->ssid);
    return ESP_OK;

err_unregister_ip_handler:
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_inst);
err_unregister_wifi_handler:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_inst);
err_deinit_wifi:
    esp_wifi_deinit();
err_destroy_netif:
    esp_netif_destroy_default_wifi(s_netif);
    s_netif = NULL;
err_delete_event_group:
    vEventGroupDelete(s_wifi_events);
    s_wifi_events = NULL;
    return ret;
}

esp_err_t wlink_sta_connect_start(void) {
    s_user_disconnect = false;
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    return esp_wifi_start();
}

esp_err_t wlink_sta_wait_connected(void) {
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(s_connect_timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    s_user_disconnect = true; // This should be set to true so the handler doesn't try to reconnect until max retries are exhausted in the case of a timeout.
    esp_wifi_stop();
    return (bits & WIFI_FAIL_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}


bool wlink_sta_is_connected(void) {
    if (s_wifi_events == NULL) return false;
    return xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT;
}

esp_err_t wlink_sta_disconnect(void) {
    if (!wlink_sta_is_connected()) return ESP_OK;

    s_user_disconnect = true;
    xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
    esp_err_t disconnect = esp_wifi_disconnect();

    EventBits_t bits = xEventGroupWaitBits(
    s_wifi_events,
    WIFI_DISCONNECTED_BIT,
    pdFALSE,
    pdFALSE,
    pdMS_TO_TICKS(s_connect_timeout_ms));

    esp_err_t stop = esp_wifi_stop();

    if (!(WIFI_DISCONNECTED_BIT & bits)) return ESP_ERR_TIMEOUT;

    return (disconnect != ESP_OK) ? disconnect : stop;
}

esp_err_t wlink_sta_deinit(void) {
    if (s_wifi_events == NULL) return ESP_OK;

    esp_err_t ret = ESP_OK;
    esp_err_t err;

    err = esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_inst);
    if (ret == ESP_OK) ret = err;
    err = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_inst);
    if (ret == ESP_OK) ret = err;
    err = esp_wifi_deinit();
    if (ret == ESP_OK) ret = err;

    esp_netif_destroy_default_wifi(s_netif);
    s_netif = NULL;
    vEventGroupDelete(s_wifi_events);
    s_wifi_events = NULL;
    s_wifi_evt_inst = NULL;
    s_ip_evt_inst = NULL;

    return ret;
}

esp_err_t wlink_sta_get_rssi(int8_t *out) {
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    int rssi;
    esp_err_t err = esp_wifi_sta_get_rssi(&rssi);
    if (err != ESP_OK) return err;
    *out = (int8_t)rssi; // RSSI should realistically never be out of bounds here on a valid value
    return ESP_OK;
}
