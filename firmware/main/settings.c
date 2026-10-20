#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NVS_NAMESPACE "door"
#define NVS_KEY "settings"
#define SETTINGS_VERSION 2   // bump when dd_settings_t changes

#define TERMINATE(field) ((field)[sizeof(field) - 1] = '\0')

static const char *TAG = "settings";

typedef struct {
    uint32_t version;
    dd_settings_t s;
} stored_t;

static dd_settings_t s_settings;   // guarded by s_lock
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_generation = 1;

static void defaults(dd_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->name, "Dumbledoor", sizeof(s->name));
    s->code.on = false;
    strlcpy(s->code.seq, "..-", sizeof(s->code.seq));   // click, click, hold
    s->code.short_ms = 400;   // the old Arduino sketches' thresholds
    s->code.long_ms = 700;
    s->code.gap_ms = 1500;
    strlcpy(s->mqtt_prefix, "dumbledoor", sizeof(s->mqtt_prefix));
}

// Whatever came out of flash, leave every string terminated and every number in range.
static void sanitize(dd_settings_t *s)
{
    TERMINATE(s->name);
    TERMINATE(s->code.seq);
    TERMINATE(s->mqtt_url);
    TERMINATE(s->mqtt_user);
    TERMINATE(s->mqtt_pass);
    TERMINATE(s->mqtt_prefix);
    TERMINATE(s->app_mqtt_url);
    TERMINATE(s->app_mqtt_user);
    TERMINATE(s->app_mqtt_pass);
    TERMINATE(s->app_url);
    TERMINATE(s->ntfy_url);
    if (!s->name[0]) {
        strlcpy(s->name, "Dumbledoor", sizeof(s->name));
    }
    if (strspn(s->code.seq, ".-") != strlen(s->code.seq)) {
        s->code.seq[0] = '\0';
    }
    if (s->code.long_ms < 100 || s->code.long_ms > 5000) {
        s->code.long_ms = 700;
    }
    if (s->code.short_ms < 100 || s->code.short_ms > s->code.long_ms) {
        s->code.short_ms = s->code.long_ms < 400 ? s->code.long_ms : 400;
    }
    if (s->code.gap_ms < 300 || s->code.gap_ms > 10000) {
        s->code.gap_ms = 1500;
    }
}

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    defaults(&s_settings);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "nothing saved yet, using defaults");
        return;
    }
    stored_t *st = malloc(sizeof(*st));   // ~1 KB, kept off the stack
    size_t len = sizeof(*st);
    if (st && nvs_get_blob(h, NVS_KEY, st, &len) == ESP_OK && len == sizeof(*st) &&
        st->version == SETTINGS_VERSION) {
        s_settings = st->s;
        sanitize(&s_settings);
    } else {
        ESP_LOGW(TAG, "saved settings missing or outdated, using defaults");
    }
    free(st);
    nvs_close(h);
}

void settings_get(dd_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

void settings_get_code(settings_code_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings.code;
    xSemaphoreGive(s_lock);
}

void settings_get_name(char *out, size_t size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_settings.name, size);
    xSemaphoreGive(s_lock);
}

uint32_t settings_generation(void)
{
    return s_generation;
}

// Makes st->s the settings and saves them. Caller holds s_lock.
static void commit(stored_t *st)
{
    st->version = SETTINGS_VERSION;
    sanitize(&st->s);
    s_settings = st->s;
    s_generation++;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, st, sizeof(*st));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
}

void settings_set(const dd_settings_t *s)
{
    stored_t *st = calloc(1, sizeof(*st));
    if (!st) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->s = *s;
    st->s.code = s_settings.code;
    commit(st);
    xSemaphoreGive(s_lock);
    free(st);
}

void settings_set_code(const settings_code_t *code)
{
    stored_t *st = calloc(1, sizeof(*st));
    if (!st) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->s = s_settings;
    st->s.code = *code;
    commit(st);
    xSemaphoreGive(s_lock);
    free(st);
}
