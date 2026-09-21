#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/i2c_types.h"
#include "soc/clk_tree_defs.h"
#include "ssd1306.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>
#include "wlink_sta.h"
#include "mqtt_svc.h"
#include "nvs_flash.h"

// I2C setup
#define I2C_PORT                I2C_NUM_0
#define PIN_SDA                 25
#define PIN_SCL                 26
#define DISPLAY_ADDR            0x3C

// ADC setup
#define MAX_CONSECUTIVE_FAULTS  5

// Misc.
#define MQTT_PUBLISH_TIMEOUT_MS 500
#define CLIENT_ID_PREFIX "haltestelle-esp-display-"

static const char* TAG = "main";

static void init_wifi(void) {
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wlink_sta_settings_t wifi_settings = WLINK_STA_DEFAULT_CONFIG();
    wlink_sta_init(&wifi_settings);
    wlink_sta_connect_start();
}

static void init_mqtt(void) {
    mqtt_svc_settings_t mqtt_settings = MQTT_SVC_DEFAULT_CONFIG();
    uint8_t mac[6];
    static char client_id[sizeof(CLIENT_ID_PREFIX) + 17]; // 17 = sizeof(MACSTR)
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(client_id, sizeof(client_id), CLIENT_ID_PREFIX MACSTR, 
        MAC2STR(mac));
    mqtt_settings.client_id = client_id;
    mqtt_settings.username = NULL;
    mqtt_settings.password = NULL;
    
    mqtt_svc_init(&mqtt_settings);
}

static ssd1306_handle_t init_display(void) {
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .scl_io_num = PIN_SCL,
        .sda_io_num = PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t i2c_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_handle));

    i2c_device_config_t display_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DISPLAY_ADDR,
        .scl_speed_hz = 400000,
    };

    i2c_master_dev_handle_t display_i2c_handle;

    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_handle, &display_config, &display_i2c_handle));

    ssd1306_handle_t display;
    ESP_ERROR_CHECK(ssd1306_init(display_i2c_handle, &display));
    return display;
}



void app_main(void)
{
    init_wifi();
    if (wlink_sta_wait_connected() != ESP_OK) {
        ESP_LOGE(TAG, "wifi failed");
        return;
    }

    init_mqtt();
    if (mqtt_svc_connect() != ESP_OK) {
        ESP_LOGE(TAG, "broker unreachable");
        return;
    }

    ESP_LOGI(TAG, "connected");
}