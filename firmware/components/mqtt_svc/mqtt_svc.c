#include "mqtt_svc.h"
#include "esp_bit_defs.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mqtt_svc";

#define MQTT_CONNECTED_BIT BIT0
#define MQTT_FAIL_BIT      BIT1
#define FLUSH_POLL_MS      10

typedef struct {
    char             *filter;   // NULL when the slot is free
    int               qos;
    mqtt_svc_msg_cb_t cb;
    void             *ctx;
} subscription_t;

static esp_mqtt_client_handle_t s_client;
static uint32_t                 s_connect_timeout_ms;
static EventGroupHandle_t       s_mqtt_events;
static bool                     s_started;

static mqtt_svc_conn_cb_t     s_on_connected;
static mqtt_svc_conn_cb_t     s_on_disconnected;
static mqtt_svc_delivery_cb_t s_on_delivery;
static void                  *s_cb_ctx;

static subscription_t    s_subs[CONFIG_MQTT_SVC_MAX_SUBSCRIPTIONS];
static SemaphoreHandle_t s_subs_lock;

// Incoming message being reassembled; only touched in the esp-mqtt task.
static size_t s_max_payload_len;
static char  *s_rx_topic;
static char  *s_rx_payload;

static atomic_uint s_expired_count;

static bool topic_matches(const char *filter, const char *topic)
{
    // Wildcards never match topics starting with $, e.g. $SYS.
    if (topic[0] == '$' && (filter[0] == '+' || filter[0] == '#'))
        return false;

    for (;;) {
        if (filter[0] == '#')
            return true;

        if (filter[0] == '+') {
            filter++;
            while (*topic && *topic != '/') topic++;
        } else {
            while (*filter && *filter != '/' && *filter == *topic) {
                filter++;
                topic++;
            }
            if ((*filter && *filter != '/') || (*topic && *topic != '/'))
                return false;
        }

        if (*filter == '\0' && *topic == '\0')
            return true;
        if (*topic == '\0')
            return strcmp(filter, "/#") == 0;   // "a/#" also matches "a"
        if (*filter == '\0')
            return false;
        filter++;
        topic++;
    }
}

static void resubscribe_all(void)
{
    for (size_t i = 0; i < CONFIG_MQTT_SVC_MAX_SUBSCRIPTIONS; i++) {
        char *filter = NULL;
        int qos = 0;
        bool oom = false;

        xSemaphoreTake(s_subs_lock, portMAX_DELAY);
        if (s_subs[i].filter) {
            filter = strdup(s_subs[i].filter);
            qos = s_subs[i].qos;
            oom = filter == NULL;
        }
        xSemaphoreGive(s_subs_lock);

        if (oom)
            ESP_LOGE(TAG, "resubscribe: out of memory");
        if (filter == NULL)
            continue;
        if (esp_mqtt_client_subscribe(s_client, filter, qos) < 0)
            ESP_LOGW(TAG, "resubscribe to \"%s\" failed", filter);
        free(filter);
    }
}

static void dispatch_message(const char *topic, const char *payload, size_t len)
{
    for (size_t i = 0; i < CONFIG_MQTT_SVC_MAX_SUBSCRIPTIONS; i++) {
        mqtt_svc_msg_cb_t cb = NULL;
        void *ctx = NULL;

        xSemaphoreTake(s_subs_lock, portMAX_DELAY);
        if (s_subs[i].filter && topic_matches(s_subs[i].filter, topic)) {
            cb = s_subs[i].cb;
            ctx = s_subs[i].ctx;
        }
        xSemaphoreGive(s_subs_lock);

        // Called without the lock so the callback may subscribe or unsubscribe.
        if (cb)
            cb(topic, payload, len, ctx);
    }
}

static void drop_rx(void)
{
    free(s_rx_topic);
    free(s_rx_payload);
    s_rx_topic = NULL;
    s_rx_payload = NULL;
}

// Messages larger than the esp-mqtt buffer arrive in several chunks; only the first carries the topic.
static void handle_data(const esp_mqtt_event_t *event)
{
    size_t total = event->total_data_len;
    size_t offset = event->current_data_offset;
    size_t len = event->data_len;

    if (offset == 0) {
        drop_rx();
        if (total > s_max_payload_len) {
            ESP_LOGW(TAG, "dropped message on \"%.*s\": %u bytes exceed max_payload_len",
                     event->topic_len, event->topic, (unsigned)total);
            return;
        }
        s_rx_topic = strndup(event->topic, event->topic_len);
        s_rx_payload = malloc(total + 1);
        if (s_rx_topic == NULL || s_rx_payload == NULL) {
            ESP_LOGE(TAG, "dropped message on \"%.*s\": out of memory", event->topic_len, event->topic);
            drop_rx();
            return;
        }
    }

    if (s_rx_payload == NULL || offset + len > total)
        return;

    memcpy(s_rx_payload + offset, event->data, len);
    if (offset + len < total)
        return;

    s_rx_payload[total] = '\0';
    dispatch_message(s_rx_topic, s_rx_payload, total);
    drop_rx();
}

static void mqtt_svc_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;
    switch ((esp_mqtt_event_id_t) id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
            resubscribe_all();
            if (s_on_connected)
                s_on_connected(s_cb_ctx);
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
            drop_rx();
            if (s_on_disconnected)
                s_on_disconnected(s_cb_ctx);
            break;
        case MQTT_EVENT_DATA:
            handle_data(event);
            break;
        case MQTT_EVENT_PUBLISHED:
            if (s_on_delivery)
                s_on_delivery(event->msg_id, true, s_cb_ctx);
            break;
        case MQTT_EVENT_DELETED:
            atomic_fetch_add(&s_expired_count, 1);
            ESP_LOGW(TAG, "message %d expired without acknowledgement", event->msg_id);
            if (s_on_delivery)
                s_on_delivery(event->msg_id, false, s_cb_ctx);
            break;
        case MQTT_EVENT_ERROR:
            xEventGroupSetBits(s_mqtt_events, MQTT_FAIL_BIT);
            break;
        default:
            break;
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
    s_max_payload_len = cfg->max_payload_len;
    s_on_connected = cfg->on_connected;
    s_on_disconnected = cfg->on_disconnected;
    s_on_delivery = cfg->on_delivery;
    s_cb_ctx = cfg->cb_ctx;

    s_client = esp_mqtt_client_init(&config);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_FAIL, TAG, "init failed");

    s_mqtt_events = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(s_mqtt_events != NULL, ESP_ERR_NO_MEM, err_destroy_client, TAG, "event group alloc");

    s_subs_lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(s_subs_lock != NULL, ESP_ERR_NO_MEM, err_delete_eventgroup, TAG, "mutex alloc");

    ESP_GOTO_ON_ERROR(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, &mqtt_svc_event_handler, NULL), err_delete_lock, TAG, "event registration failed");

    return ESP_OK;

err_delete_lock:
    vSemaphoreDelete(s_subs_lock);
    s_subs_lock = NULL;
err_delete_eventgroup:
    vEventGroupDelete(s_mqtt_events);
    s_mqtt_events = NULL;
err_destroy_client:
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    return ret;
}

esp_err_t mqtt_svc_connect(void) {
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
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

int mqtt_svc_publish(const char *topic, const void *payload, size_t len, int qos, bool retain)
{
    if (s_client == NULL) {
        ESP_LOGE(TAG, "publish: not initialized");
        return -1;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, (int)len, qos, (int)retain);
    if (msg_id < 0)
        ESP_LOGE(TAG, "publish rejected by client: topic=\"%s\" qos=%d len=%u", topic, qos, (unsigned)len);
    return msg_id;
}

esp_err_t mqtt_svc_flush(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    unsigned expired = atomic_load(&s_expired_count);
    TickType_t start = xTaskGetTickCount();

    // Expired messages leave the outbox too, so an empty outbox alone is not success.
    while (esp_mqtt_client_get_outbox_size(s_client) > 0) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGW(TAG, "flush timed out after %"PRIu32" ms", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(FLUSH_POLL_MS));
    }

    if (atomic_load(&s_expired_count) != expired) {
        ESP_LOGW(TAG, "flush: messages expired without acknowledgement");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mqtt_svc_subscribe(const char *filter, int qos, mqtt_svc_msg_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(filter != NULL && cb != NULL, ESP_ERR_INVALID_ARG, TAG, "filter and cb are required");

    char *copy = strdup(filter);
    ESP_RETURN_ON_FALSE(copy != NULL, ESP_ERR_NO_MEM, TAG, "subscribe: out of memory");

    subscription_t *slot = NULL;
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);
    for (size_t i = 0; i < CONFIG_MQTT_SVC_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].filter && strcmp(s_subs[i].filter, filter) == 0) {
            slot = &s_subs[i];
            break;
        }
        if (slot == NULL && s_subs[i].filter == NULL)
            slot = &s_subs[i];
    }
    if (slot) {
        free(slot->filter);
        *slot = (subscription_t){ .filter = copy, .qos = qos, .cb = cb, .ctx = ctx };
    }
    xSemaphoreGive(s_subs_lock);

    if (slot == NULL) {
        free(copy);
        ESP_LOGE(TAG, "no free subscription slot for \"%s\"", filter);
        return ESP_ERR_NO_MEM;
    }

    // While disconnected, resubscribe_all() sends it on connect.
    if (mqtt_svc_is_connected() && esp_mqtt_client_subscribe(s_client, filter, qos) < 0)
        ESP_LOGW(TAG, "subscribe to \"%s\" failed, retried on next connect", filter);
    return ESP_OK;
}

esp_err_t mqtt_svc_unsubscribe(const char *filter)
{
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(filter != NULL, ESP_ERR_INVALID_ARG, TAG, "filter is required");

    char *removed = NULL;
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);
    for (size_t i = 0; i < CONFIG_MQTT_SVC_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].filter && strcmp(s_subs[i].filter, filter) == 0) {
            removed = s_subs[i].filter;
            s_subs[i] = (subscription_t){ 0 };
            break;
        }
    }
    xSemaphoreGive(s_subs_lock);

    if (removed == NULL)
        return ESP_ERR_NOT_FOUND;
    free(removed);

    if (mqtt_svc_is_connected() && esp_mqtt_client_unsubscribe(s_client, filter) < 0)
        ESP_LOGW(TAG, "unsubscribe from \"%s\" failed", filter);
    return ESP_OK;
}

esp_err_t mqtt_svc_disconnect(void) {
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_disconnect(s_client), TAG, "error while disconnecting");
    return ESP_OK;
}

bool mqtt_svc_is_connected(void) {
    if (s_mqtt_events == NULL) return false;
    return xEventGroupGetBits(s_mqtt_events) & MQTT_CONNECTED_BIT;
}

esp_mqtt_client_handle_t mqtt_svc_client(void) {
    return s_client;
}

esp_err_t mqtt_svc_deinit(void) {
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

    for (size_t i = 0; i < CONFIG_MQTT_SVC_MAX_SUBSCRIPTIONS; i++) {
        free(s_subs[i].filter);
        s_subs[i] = (subscription_t){ 0 };
    }
    vSemaphoreDelete(s_subs_lock);
    s_subs_lock = NULL;

    drop_rx();
    atomic_store(&s_expired_count, 0);

    return ret;
}
