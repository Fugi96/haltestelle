#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "hal/i2c_types.h"
#include "soc/clk_tree_defs.h"
#include "ssd1306.h"
#include "gfx.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "wlink_sta.h"
#include "mqtt_svc.h"
#include "nvs_flash.h"

// I2C setup
#define I2C_PORT                I2C_NUM_0
#define PIN_SDA                 25
#define PIN_SCL                 26
#define DISPLAY_ADDR            0x3C
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64

// ADC setup
#define MAX_CONSECUTIVE_FAULTS  5

// Misc.
#define CLIENT_ID_PREFIX "haltestelle-esp-display-"
#define TOPIC_PREFIX     "haltestelle/"
#define TIMEZONE         "CET-1CEST,M3.5.0,M10.5.0/3"

// Layout
#define MAX_DEPARTURES   6
#define ROW_HEIGHT       10
#define COL_GAP          4    // between line number, destination and countdown
#define TICK_MS          1000

// Destinations are shortened only as far as the row needs, in three steps: the words below,
// then cutting words after their first syllable, then "..." from the renderer.
#define MAX_SEGMENTS     12   // words a destination is split into
#define MAX_SEGMENT_LEN  24
#define MIN_KEPT_CHARS   4    // before the period, so "Universität" is not cut to "Un."

static const struct { const char *from; const char *to; } WORDS[] = {
    { "Hauptbahnhof", "Hbf"    },
    { "Bahnhof",      "Bf"     },
    { "Betriebshof",  "Btf"    },
    { "Krankenhaus",  "Krhs."  },
    { "Friedhof",     "Frdh."  },
    { "Flughafen",    "Flugh." },
    { "Universität",  "Uni"    },
    { "Straße",       "Str."   },
    { "Platz",        "Pl."    },
};

// Endings of compound words; only used when something is left in front of them.
static const struct { const char *from; const char *to; } ENDINGS[] = {
    { "straße",   "str."  },
    { "platz",    "pl."   },
    { "bahnhof",  "bf"    },
    { "friedhof", "frdh." },
    { "brücke",   "br."   },
};

#define EV_DEPARTURES    BIT0

typedef struct {
    char    line[8];
    char    dest[48];
    int64_t ts;
    bool    rt;
} departure_t;

static const char* TAG = "main";

static gfx_color_t s_framebuffer[GFX_BUF_LEN(DISPLAY_WIDTH, DISPLAY_HEIGHT)];
static gfx_canvas_t s_canvas = { s_framebuffer, DISPLAY_WIDTH, DISPLAY_HEIGHT };
static ssd1306_handle_t s_display;

// Owned by the display task; the board replaces them as a whole.
static departure_t s_departures[MAX_DEPARTURES];
static int s_departure_count;

static TaskHandle_t      s_display_task;
static SemaphoreHandle_t s_board_lock;
static char             *s_board_json;   // newest unparsed board, under s_board_lock

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

static void log_message(const char *topic, const char *payload, size_t len, void *ctx) {
    ESP_LOGI(TAG, "%s (%u bytes): %s", topic, (unsigned)len, payload);
}

// Runs in the esp-mqtt task: copy and wake, parsing happens in the display task.
static void on_board(const char *topic, const char *payload, size_t len, void *ctx) {
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        ESP_LOGE(TAG, "board dropped: out of memory");
        return;
    }
    memcpy(copy, payload, len + 1);

    xSemaphoreTake(s_board_lock, portMAX_DELAY);
    free(s_board_json);   // an unprocessed older board is superseded
    s_board_json = copy;
    xSemaphoreGive(s_board_lock);

    xTaskNotify(s_display_task, EV_DEPARTURES, eSetBits);
}

static esp_err_t init_mqtt(void) {
    mqtt_svc_settings_t mqtt_settings = MQTT_SVC_DEFAULT_CONFIG();
    uint8_t mac[6];
    static char client_id[sizeof(CLIENT_ID_PREFIX) + 17]; // 17 = sizeof(MACSTR)
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(client_id, sizeof(client_id), CLIENT_ID_PREFIX MACSTR,
        MAC2STR(mac));
    mqtt_settings.client_id = client_id;
    mqtt_settings.username = NULL;
    mqtt_settings.password = NULL;

    return mqtt_svc_init(&mqtt_settings);
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

// The board's own build time is the only clock this device has.
static void set_clock(int64_t gen) {
    struct timeval tv = { .tv_sec = (time_t)gen, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

#define ARRAY_LEN(a) (sizeof(a) / sizeof(*(a)))

typedef struct {
    char text[MAX_SEGMENT_LEN];
    char separator;   // what followed this word, '\0' at the end
} segment_t;

static int utf8_len(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

static bool is_vowel(const char *s) {
    if (strchr("aeiouyAEIOUY", *s) && *s)
        return true;
    // ä ö ü and their capitals, the only two-byte vowels German destinations use.
    return (unsigned char)s[0] == 0xC3 && strchr("\xA4\xB6\xBC\x84\x96\x9C", s[1]) != NULL;
}

static int split_words(const char *src, segment_t *segments, int max_segments) {
    int count = 0;
    size_t len = 0;

    for (; *src && count < max_segments; src++) {
        if (strchr(" -/", *src)) {
            segments[count].text[len] = '\0';
            segments[count++].separator = *src;
            len = 0;
        } else if (len + 1 < MAX_SEGMENT_LEN) {
            segments[count].text[len++] = *src;
        }
    }
    if (count < max_segments) {
        segments[count].text[len] = '\0';
        segments[count++].separator = '\0';
    }
    return count;
}

static void join_words(char *out, size_t size, const segment_t *segments, int count) {
    size_t at = 0;
    for (int i = 0; i < count && at + 1 < size; i++)
        at += snprintf(out + at, size - at, "%s%s", segments[i].text,
                       segments[i].separator ? (char[2]){ segments[i].separator, 0 } : "");
}

// "Hauptbahnhof" -> "Hbf", "Bilkerstraße" -> "Bilkerstr."
static void apply_dictionary(char *word) {
    for (size_t i = 0; i < ARRAY_LEN(WORDS); i++) {
        if (strcmp(word, WORDS[i].from) == 0) {
            snprintf(word, MAX_SEGMENT_LEN, "%s", WORDS[i].to);
            return;
        }
    }
    size_t len = strlen(word);
    for (size_t i = 0; i < ARRAY_LEN(ENDINGS); i++) {
        size_t ending = strlen(ENDINGS[i].from);
        if (len > ending + 2 && strcmp(word + len - ending, ENDINGS[i].from) == 0) {
            snprintf(word + len - ending, MAX_SEGMENT_LEN - (len - ending), "%s", ENDINGS[i].to);
            return;
        }
    }
}

// Cuts after the first vowel group and the consonants behind it: "Monheimer" -> "Monh."
static bool clip_word(char *word) {
    const char *at = word;
    int chars = 0;

    while (*at && !is_vowel(at)) { at += utf8_len(at); chars++; }
    while (*at && is_vowel(at))  { at += utf8_len(at); chars++; }
    while (*at && !is_vowel(at)) { at += utf8_len(at); chars++; }
    while (*at && chars < MIN_KEPT_CHARS) { at += utf8_len(at); chars++; }

    size_t keep = (size_t)(at - word);
    if (*at == '\0' || keep + 1 >= strlen(word))
        return false;   // nothing left to cut, or the period would cost what it saves

    word[keep] = '.';
    word[keep + 1] = '\0';
    return true;
}

// Shortens dest until it fits max_width; what still does not fit the renderer cuts with "...".
static void fit_destination(const char *dest, int max_width, char *out, size_t size) {
    segment_t segments[MAX_SEGMENTS];
    int count = split_words(dest, segments, MAX_SEGMENTS);

    join_words(out, size, segments, count);
    if (gfx_text_width(out) <= max_width)
        return;

    for (int i = 0; i < count; i++)
        apply_dictionary(segments[i].text);
    join_words(out, size, segments, count);
    if (gfx_text_width(out) <= max_width)
        return;

    // Left to right, so the last word, the one that names the place, is cut last.
    for (int i = 0; i < count; i++) {
        if (!clip_word(segments[i].text))
            continue;
        join_words(out, size, segments, count);
        if (gfx_text_width(out) <= max_width)
            return;
    }
}

static void parse_board(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "board dropped: invalid json");
        return;
    }

    const cJSON *gen = cJSON_GetObjectItem(root, "gen");
    if (cJSON_IsNumber(gen))
        set_clock((int64_t)cJSON_GetNumberValue(gen));

    s_departure_count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "departures")) {
        if (s_departure_count >= MAX_DEPARTURES)
            break;

        const cJSON *line = cJSON_GetObjectItem(item, "line");
        const cJSON *dest = cJSON_GetObjectItem(item, "dest");
        const cJSON *ts = cJSON_GetObjectItem(item, "ts");
        if (!cJSON_IsString(line) || !cJSON_IsString(dest) || !cJSON_IsNumber(ts))
            continue;

        departure_t *d = &s_departures[s_departure_count++];
        snprintf(d->line, sizeof(d->line), "%s", cJSON_GetStringValue(line));
        snprintf(d->dest, sizeof(d->dest), "%s", cJSON_GetStringValue(dest));
        d->ts = (int64_t)cJSON_GetNumberValue(ts);
        d->rt = cJSON_IsTrue(cJSON_GetObjectItem(item, "rt"));
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "board: %d departures", s_departure_count);
}

// Minutes switch at the half minute: 0:30..1:29 is "1 Min", below that "sofort".
static void format_countdown(const departure_t *d, time_t now, char *out, size_t size) {
    if (!d->rt) {
        struct tm tm;
        time_t ts = (time_t)d->ts;
        localtime_r(&ts, &tm);
        strftime(out, size, "%H:%M", &tm);
        return;
    }

    long left = (long)(d->ts - (int64_t)now);
    if (left < 30)
        snprintf(out, size, "sofort");
    else
        snprintf(out, size, "%ld Min", (left + 30) / 60);
}

static void render(void) {
    time_t now = time(NULL);

    // Destinations line up under each other, but only as far right as the board needs.
    int dest_x = 0;
    for (int i = 0; i < s_departure_count; i++) {
        int w = gfx_text_width(s_departures[i].line);
        if (w > dest_x)
            dest_x = w;
    }
    dest_x += COL_GAP;

    gfx_clear(&s_canvas);
    for (int i = 0; i < s_departure_count; i++) {
        const departure_t *d = &s_departures[i];
        int y = i * ROW_HEIGHT;

        char countdown[16];
        format_countdown(d, now, countdown, sizeof(countdown));
        int countdown_x = DISPLAY_WIDTH - gfx_text_width(countdown);

        gfx_draw_string(&s_canvas, d->line, 0, y, GFX_AMBER);
        gfx_draw_string(&s_canvas, countdown, countdown_x, y, GFX_AMBER);
        int dest_width = countdown_x - COL_GAP - dest_x;
        char dest[sizeof(d->dest)];
        fit_destination(d->dest, dest_width, dest, sizeof(dest));
        gfx_draw_string_ellipsized(&s_canvas, dest, dest_x, y, dest_width, GFX_AMBER);
    }

    ESP_ERROR_CHECK(ssd1306_flush(s_display, s_canvas.buf, s_canvas.width, s_canvas.height));
}

static void display_task(void *arg) {
    ESP_ERROR_CHECK(mqtt_svc_subscribe(TOPIC_PREFIX "departures", 1, on_board, NULL));
    ESP_ERROR_CHECK(mqtt_svc_subscribe(TOPIC_PREFIX "power", 1, log_message, NULL));
    ESP_ERROR_CHECK(mqtt_svc_subscribe(TOPIC_PREFIX "settings", 1, log_message, NULL));
    ESP_ERROR_CHECK(mqtt_svc_subscribe(TOPIC_PREFIX "status", 1, log_message, NULL));

    if (mqtt_svc_connect() == ESP_OK)
        ESP_LOGI(TAG, "connected");
    else
        ESP_LOGE(TAG, "broker unreachable, retrying in the background");

    for (;;) {
        uint32_t changed;
        if (xTaskNotifyWait(0, UINT32_MAX, &changed, pdMS_TO_TICKS(TICK_MS)) != pdTRUE)
            changed = 0;   // timeout: no new board, but the countdowns move on

        if (changed & EV_DEPARTURES) {
            xSemaphoreTake(s_board_lock, portMAX_DELAY);
            char *json = s_board_json;
            s_board_json = NULL;
            xSemaphoreGive(s_board_lock);

            if (json) {
                parse_board(json);
                free(json);
            }
        }

        render();
    }
}

void app_main(void)
{
    setenv("TZ", TIMEZONE, 1);
    tzset();

    s_display = init_display();
    gfx_clear(&s_canvas);
    ESP_ERROR_CHECK(ssd1306_flush(s_display, s_canvas.buf, s_canvas.width, s_canvas.height));

    init_wifi();
    if (wlink_sta_wait_connected() != ESP_OK) {
        ESP_LOGE(TAG, "wifi failed");
        return;
    }

    if (init_mqtt() != ESP_OK) {
        ESP_LOGE(TAG, "mqtt init failed, check the broker uri");
        return;
    }

    s_board_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_board_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreate(display_task, "display", 8192, NULL, 5, &s_display_task);
}
