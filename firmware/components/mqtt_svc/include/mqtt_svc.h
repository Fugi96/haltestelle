#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// for documentation: broker_uri must be a valid non-null null-terminated c-string, e.g. "mqtt://192.168.1.10:1883"
// for documentation: client_id may be null (broker assigns one) or a null-terminated c-string
// for documentation: username/password may both be null (anonymous broker) or valid null-terminated c-strings
// for documentation: lifecycle: init -> connect -> (publish_and_confirm)* -> disconnect -> deinit
// for documentation: requires an established IP connection (wlink connected + got IP) before connect is called
// for documentation: nvs and the default event loop need to be initialized

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *topic;
    const char *msg;
    int         qos;
    bool        retain;
} mqtt_svc_lwt_t;

typedef struct {
    const char    *broker_uri;
    const char    *client_id;
    const char    *username;
    const char    *password;
    uint32_t       connect_timeout_ms;   // how long connect() waits for MQTT_EVENT_CONNECTED
    mqtt_svc_lwt_t last_will;
} mqtt_svc_settings_t;

#define MQTT_SVC_DEFAULT_CONFIG() (mqtt_svc_settings_t){ \
    .broker_uri         = CONFIG_MQTT_SVC_BROKER_URI,    \
    .username           = CONFIG_MQTT_SVC_USERNAME,      \
    .password           = CONFIG_MQTT_SVC_PASSWORD,      \
    .connect_timeout_ms = 10000,                         \
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

// Publishes payload to topic and BLOCKS until the message is confirmed on the wire,
// or timeout_ms elapses.
//   qos 0: returns once the client reports the bytes were handed to the transport.
//   qos 1: returns only after the broker's PUBACK (MQTT_EVENT_PUBLISHED).
// This is the "is it safe to deep-sleep yet?" gate: a success return means the reading
// is delivered; ESP_ERR_TIMEOUT means it is NOT, and the caller decides retry vs. drop.
// payload is treated as raw bytes; pass len explicitly (0 => use strlen for c-strings).
esp_err_t mqtt_svc_publish(const char *topic,
                                       const void *payload,
                                       size_t      len,
                                       int         qos,
                                       bool        retain,
                                       int         timeout_ms);

// Cleanly closes the connection (DISCONNECT). Safe to call whether or not connected.
esp_err_t mqtt_svc_disconnect(void);

// Destroys the client and frees resources. Lifecycle back to pre-init.
esp_err_t mqtt_svc_deinit(void);

// May be called at any time after init.
bool mqtt_svc_is_connected(void);

#ifdef __cplusplus
}
#endif
