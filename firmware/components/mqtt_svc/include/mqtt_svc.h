#pragma once
#include "esp_err.h"
#include "mqtt_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// for documentation: broker_uri must be a valid non-null null-terminated c-string, e.g. "mqtt://192.168.1.10:1883"
// for documentation: client_id may be null (broker assigns one) or a null-terminated c-string
// for documentation: username/password may both be null (anonymous broker) or valid null-terminated c-strings
// for documentation: lifecycle: init -> (subscribe)* -> connect -> (publish | subscribe | unsubscribe | flush)* -> disconnect -> deinit
// for documentation: requires an established IP connection (wlink connected + got IP) before connect is called
// for documentation: nvs and the default event loop need to be initialized
// for documentation: all callbacks run in the esp-mqtt task and must not block; publish is fine, flush is not

#ifdef __cplusplus
extern "C" {
#endif

// payload is null-terminated for convenience; len excludes the terminator.
typedef void (*mqtt_svc_msg_cb_t)(const char *topic, const char *payload, size_t len, void *ctx);
typedef void (*mqtt_svc_conn_cb_t)(void *ctx);
// delivered is false when the message expired in the outbox without an acknowledgement.
typedef void (*mqtt_svc_delivery_cb_t)(int msg_id, bool delivered, void *ctx);

typedef struct {
    const char *topic;
    const char *msg;
    int         qos;
    bool        retain;
} mqtt_svc_lwt_t;

typedef struct {
    const char            *broker_uri;
    const char            *client_id;
    const char            *username;
    const char            *password;
    uint32_t               connect_timeout_ms;   // how long connect() waits for MQTT_EVENT_CONNECTED
    size_t                 max_payload_len;      // larger incoming messages are dropped
    mqtt_svc_lwt_t         last_will;
    mqtt_svc_conn_cb_t     on_connected;         // optional, also runs after automatic reconnects
    mqtt_svc_conn_cb_t     on_disconnected;      // optional
    mqtt_svc_delivery_cb_t on_delivery;          // optional, QoS 1 and 2 only
    void                  *cb_ctx;               // passed to the three callbacks above
} mqtt_svc_settings_t;

#define MQTT_SVC_DEFAULT_CONFIG() (mqtt_svc_settings_t){ \
    .broker_uri         = CONFIG_MQTT_SVC_BROKER_URI,    \
    .username           = CONFIG_MQTT_SVC_USERNAME,      \
    .password           = CONFIG_MQTT_SVC_PASSWORD,      \
    .connect_timeout_ms = 10000,                         \
    .max_payload_len    = 8192,                          \
    .last_will          = {                              \
        .topic  = NULL,                                  \
        .msg    = NULL,                                  \
        .qos    = 1,                                     \
        .retain = true,                                  \
    }                                                    \
}

// Creates and configures the underlying esp-mqtt client. Does NOT open the connection.
esp_err_t mqtt_svc_init(const mqtt_svc_settings_t *cfg);

// Starts the client and BLOCKS until MQTT_EVENT_CONNECTED or connect_timeout_ms elapses.
// Returns ESP_ERR_TIMEOUT if the broker was not reached in time.
esp_err_t mqtt_svc_connect(void);

// Publishes payload to topic without waiting for the acknowledgement.
// QoS 1 and 2 messages stay in the esp-mqtt outbox until acknowledged, also across reconnects,
// and are reported through on_delivery. len 0 means payload is a c-string.
// Returns the msg_id (0 for QoS 0), or a negative value if the message was not accepted.
int mqtt_svc_publish(const char *topic, const void *payload, size_t len, int qos, bool retain);

// BLOCKS until every QoS 1 and 2 message published so far is acknowledged, e.g. before deep sleep.
// Returns ESP_ERR_TIMEOUT if the outbox did not drain in time, ESP_FAIL if a message expired meanwhile.
esp_err_t mqtt_svc_flush(uint32_t timeout_ms);

// Calls cb for every incoming message matching filter (+ and # wildcards allowed).
// Subscriptions are kept across reconnects; subscribing again with the same filter replaces cb.
// Returns ESP_ERR_NO_MEM when all CONFIG_MQTT_SVC_MAX_SUBSCRIPTIONS slots are taken.
esp_err_t mqtt_svc_subscribe(const char *filter, int qos, mqtt_svc_msg_cb_t cb, void *ctx);

// Returns ESP_ERR_NOT_FOUND if filter was not subscribed.
esp_err_t mqtt_svc_unsubscribe(const char *filter);

// Cleanly closes the connection (DISCONNECT). Safe to call whether or not connected.
esp_err_t mqtt_svc_disconnect(void);

// Destroys the client, drops all subscriptions and frees resources. Lifecycle back to pre-init.
esp_err_t mqtt_svc_deinit(void);

// May be called at any time after init.
bool mqtt_svc_is_connected(void);

// The underlying esp-mqtt client, for anything this component does not cover. NULL outside init..deinit.
// Do not start, stop or destroy the client through it.
esp_mqtt_client_handle_t mqtt_svc_client(void);

#ifdef __cplusplus
}
#endif
