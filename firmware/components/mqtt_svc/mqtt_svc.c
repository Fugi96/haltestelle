#include "mqtt_svc.h"
#include "esp_bit_defs.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include "portmacro.h"
#include <string.h>

static const char *TAG = "mqtt_svc";

#define MQTT_CONNECTED_BIT BIT0
#define MQTT_FAIL_BIT      BIT1
#define MAX_CONCURRENT_MESSAGES 16

typedef struct {
    int msg_id;
    TaskHandle_t waiter;
    bool in_use;
} inflight_slot_t;

static esp_mqtt_client_handle_t s_client;
static uint32_t s_connect_timeout_ms;
static EventGroupHandle_t s_mqtt_events;
static inflight_slot_t s_inflight_msgs[MAX_CONCURRENT_MESSAGES];
static SemaphoreHandle_t s_inflight_msgs_lock;
static bool s_started;

static void mqtt_svc_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event_data = data;
    switch ((esp_mqtt_event_id_t) id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_PUBLISHED: {
            int msg_id = event_data->msg_id;
            xSemaphoreTake(s_inflight_msgs_lock, portMAX_DELAY);
            for (size_t i = 0; i < MAX_CONCURRENT_MESSAGES; i++) {
                if (s_inflight_msgs[i].in_use && s_inflight_msgs[i].msg_id == msg_id) {
                    s_inflight_msgs[i].in_use = false;
                    xTaskNotifyGive(s_inflight_msgs[i].waiter);
                    xSemaphoreGive(s_inflight_msgs_lock);
                    return;
                }
            }
            xSemaphoreGive(s_inflight_msgs_lock);
            ESP_LOGW(TAG, "Received PUBACK for unknown message id %d", msg_id);
            break;
        }
        case MQTT_EVENT_ERROR:
            xEventGroupSetBits(s_mqtt_events, MQTT_FAIL_BIT);
            break;
        default:
            return;
    }
}

esp_err_t mqtt_svc_init(const mqtt_svc_settings_t *cfg) {
    esp_err_t ret = ESP_OK;

    esp_mqtt_client_config_t config = {
        .broker.address.uri                  = cfg->broker_uri,
        .credentials.username                = cfg->username,
        .credentials.client_id               = cfg->client_id,
        .credentials.authentication.password = cfg->password,
        .session.last_will = {
            .topic  = cfg->last_will.topic,
            .msg    = cfg->last_will.msg,
            .qos    = cfg->last_will.qos,
            .retain = cfg->last_will.retain,
        },
    };
    
    s_connect_timeout_ms = cfg->connect_timeout_ms;

    s_client = esp_mqtt_client_init(&config);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_FAIL, TAG, "init failed");

    s_mqtt_events = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(s_mqtt_events != NULL, ESP_ERR_NO_MEM, err_destroy_client, TAG, "event group alloc");

    ESP_GOTO_ON_ERROR(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, &mqtt_svc_event_handler, NULL), err_delete_eventgroup, TAG, "event registration failed");
    
    s_inflight_msgs_lock = xSemaphoreCreateMutex();
    if (s_inflight_msgs_lock == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err_delete_eventgroup;
    }

    return ESP_OK;

err_delete_eventgroup:
    vEventGroupDelete(s_mqtt_events);
    s_mqtt_events = NULL;
err_destroy_client:
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    return ret;
}

esp_err_t mqtt_svc_connect(void) {
    if (mqtt_svc_is_connected())
        return ESP_OK;

    xEventGroupClearBits(s_mqtt_events, MQTT_FAIL_BIT);

    if (s_started)
        ESP_RETURN_ON_ERROR(esp_mqtt_client_reconnect(s_client), TAG, "connection failed");
    else
    {
        ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "connection failed");
        s_started = true;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_mqtt_events, 
        MQTT_CONNECTED_BIT | MQTT_FAIL_BIT, 
        pdFALSE, 
        pdFALSE, 
        pdMS_TO_TICKS(s_connect_timeout_ms));
    
    if (bits & MQTT_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to mqtt broker");
        return ESP_OK;
    }

    if (bits & MQTT_FAIL_BIT) {
        ESP_LOGE(TAG, "connect failed: broker refused connection");
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "connect timed out after %"PRIu32" ms", s_connect_timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t mqtt_svc_publish(const char *topic,
                                       const void *payload,
                                       size_t      len,
                                       int         qos,
                                       bool        retain,
                                       int         timeout_ms) 
{
    if (qos == 0 || timeout_ms <= 0) {
        int id = esp_mqtt_client_publish(s_client, topic, payload, len, qos, (int)retain);
        return (id < 0) ? ESP_FAIL : ESP_OK;
    }

    xSemaphoreTake(s_inflight_msgs_lock, portMAX_DELAY);

    size_t slot = 0;
    while (slot < MAX_CONCURRENT_MESSAGES && s_inflight_msgs[slot].in_use) {
        slot++;
    }

    if (slot >= MAX_CONCURRENT_MESSAGES) {
        xSemaphoreGive(s_inflight_msgs_lock);
        ESP_LOGW(TAG, "message with topic \"%s\" not published (no available message slot)", topic);
        return ESP_FAIL;
    }

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, len, qos, (int)retain);
    if (msg_id == -1) {
        xSemaphoreGive(s_inflight_msgs_lock);
        ESP_LOGE(TAG, "publish rejected by client: topic=\"%s\" qos=%d len=%u",
            topic, qos, (unsigned)len);
        return ESP_FAIL;
    }
    
    s_inflight_msgs[slot].in_use = true;
    s_inflight_msgs[slot].msg_id = msg_id;
    s_inflight_msgs[slot].waiter = xTaskGetCurrentTaskHandle();

    xSemaphoreGive(s_inflight_msgs_lock);

    uint32_t wait = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (wait == 0) {
        xSemaphoreTake(s_inflight_msgs_lock, portMAX_DELAY);
        if (s_inflight_msgs[slot].in_use && s_inflight_msgs[slot].msg_id == msg_id) {
            s_inflight_msgs[slot].in_use = false;
            xSemaphoreGive(s_inflight_msgs_lock);
            ESP_LOGE(TAG, "publish timeout: topic=\"%s\" msg_id=%d after %d ms", topic, msg_id, qos, timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreGive(s_inflight_msgs_lock);
        ulTaskNotifyTake(pdTRUE, 0);
    }
    return ESP_OK;
}

esp_err_t mqtt_svc_disconnect(void) {
    ESP_RETURN_ON_ERROR(esp_mqtt_client_disconnect(s_client), TAG, "error while disconnecting");
    return ESP_OK;
}

bool mqtt_svc_is_connected(void) {
    if (s_mqtt_events == NULL) return false;
    return xEventGroupGetBits(s_mqtt_events) & MQTT_CONNECTED_BIT;
}

esp_err_t mqtt_svc_deinit() {
    if (s_client == NULL) {
        ESP_LOGW(TAG, "called mqtt_svc_deinit on unitialized mqtt client");
        return ESP_OK;
    }
    esp_err_t ret = ESP_OK;
    esp_err_t err;
    err = esp_mqtt_client_stop(s_client);
    if (ret == ESP_OK) ret = err;
    s_started = false;
    err = esp_mqtt_client_unregister_event(s_client, MQTT_EVENT_ANY, &mqtt_svc_event_handler);
    if (ret == ESP_OK) ret = err;
    err = esp_mqtt_client_destroy(s_client);
    if (ret == ESP_OK) ret = err;
    s_client = NULL;

    vEventGroupDelete(s_mqtt_events);
    s_mqtt_events = NULL;

    vSemaphoreDelete(s_inflight_msgs_lock);
    s_inflight_msgs_lock = NULL;


    memset(s_inflight_msgs, 0, MAX_CONCURRENT_MESSAGES*sizeof(inflight_slot_t));

    return ret;
}