#include "esp_err.h"
#include "esp_event.h"
#include "unity.h"
#include "unity_config.h"
#include "wlink_sta.h"
#include "nvs_flash.h"

TEST_CASE("init, connect, disconnect, deinit lifecycle", "[wlink_sta][integration]")
{
    wlink_sta_settings_t settings = WLINK_STA_DEFAULT_CONFIG();
    TEST_ESP_OK(wlink_sta_init(&settings));

    esp_err_t connect      = wlink_sta_connect_start();
    esp_err_t wait         = wlink_sta_wait_connected();
    bool      connected    = wlink_sta_is_connected();

    esp_err_t disconnect   = wlink_sta_disconnect();
    bool      disconnected = !wlink_sta_is_connected();

    esp_err_t deinit       = wlink_sta_deinit();

    TEST_ESP_OK(connect);
    TEST_ESP_OK(wait);
    TEST_ASSERT_TRUE(connected);
    TEST_ESP_OK(disconnect);
    TEST_ASSERT_TRUE(disconnected);
    TEST_ESP_OK(deinit);
}

TEST_CASE("init rejects NULL config", "[wlink_sta]")
{
    TEST_ESP_ERR(ESP_ERR_INVALID_ARG, wlink_sta_init(NULL));
}

TEST_CASE("get_rssi rejects NULL out pointer", "[wlink_sta]")
{
    TEST_ESP_ERR(ESP_ERR_INVALID_ARG, wlink_sta_get_rssi(NULL));
}

TEST_CASE("is_connected is false before init", "[wlink_sta]")
{
    TEST_ASSERT_FALSE(wlink_sta_is_connected());
}

void app_main(void)
{
    esp_event_loop_create_default();
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    unity_run_menu();
    esp_event_loop_delete_default();
}
