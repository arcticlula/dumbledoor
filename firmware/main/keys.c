#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "events.h"
#include "keys.h"
#include "util.h"

#define NVS_NAMESPACE "door"
#define NVS_KEY "keys"
#define KEYS_VERSION 1                         // bump when stored_t changes
#define MAX_CLOCK_SKEW_MS (2 * 60 * 1000)      // how far a command's timestamp may be from ours
#define FIRST_CLAIM_WINDOW_MS (30 * 60 * 1000)

static const char *TAG = "keys";

typedef struct {
    uint32_t id;
    char name[KEY_NAME_LEN];
    uint8_t admin;
    uint8_t secret[KEY_SECRET_LEN];
    int64_t last_t;
} stored_key_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    stored_key_t keys[KEYS_MAX];
} stored_t;

static stored_t s_db;              // guarded by s_lock
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_claim_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_claim_open;          // guarded by s_claim_mux
static uint32_t s_claim_until;

// Caller holds s_lock.
static void save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &s_db, sizeof(s_db));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
}

// Caller holds s_lock.
static stored_key_t *find(uint32_t id)
{
    for (uint32_t i = 0; i < s_db.count; i++) {
        if (s_db.keys[i].id == id) {
            return &s_db.keys[i];
        }
    }
    return NULL;
}

static void to_info(const stored_key_t *k, key_info_t *out)
{
    out->id = k->id;
    strlcpy(out->name, k->name, sizeof(out->name));
    out->admin = k->admin;
    out->last_t = k->last_t;
}

static bool parse_id(const char *s, uint32_t *out)
{
    uint8_t b[4];
    if (!util_unhex(s, b, sizeof(b))) {
        return false;
    }
    *out = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    return true;
}

static void post_key_event(uint8_t type, const char *name)
{
    event_t e = { .type = type };
    strlcpy(e.who, name, sizeof(e.who));
    events_post(&e);
}

void keys_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_db);
        if (nvs_get_blob(h, NVS_KEY, &s_db, &len) != ESP_OK || len != sizeof(s_db) ||
            s_db.version != KEYS_VERSION || s_db.count > KEYS_MAX) {
            ESP_LOGW(TAG, "saved keys missing or outdated");
            memset(&s_db, 0, sizeof(s_db));
        }
        nvs_close(h);
    }
    s_db.version = KEYS_VERSION;
    for (uint32_t i = 0; i < s_db.count; i++) {
        s_db.keys[i].name[KEY_NAME_LEN - 1] = '\0';
    }
    ESP_LOGI(TAG, "%" PRIu32 " phone key(s)", s_db.count);
    if (s_db.count == 0) {
        keys_allow_claim(FIRST_CLAIM_WINDOW_MS);
    }
}

int keys_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_db.count;
    xSemaphoreGive(s_lock);
    return n;
}

bool keys_exists(uint32_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = find(id) != NULL;
    xSemaphoreGive(s_lock);
    return found;
}

int keys_list(key_info_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (uint32_t i = 0; i < s_db.count && n < max; i++) {
        to_info(&s_db.keys[i], &out[n++]);
    }
    xSemaphoreGive(s_lock);
    return n;
}

// The checks after the key is known. Caller holds s_lock.
static auth_result_t check(stored_key_t *key, const char *body, const uint8_t *sig, cJSON **payload)
{
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key->secret, sizeof(key->secret),
                    (const unsigned char *)body, strlen(body), mac);
    if (mbedtls_ct_memcmp(mac, sig, sizeof(mac)) != 0) {
        return AUTH_BAD_SIGNATURE;
    }

    cJSON *p = cJSON_Parse(body);
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(p, "d");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(p, "t");
    auth_result_t result = AUTH_OK;
    if (!cJSON_IsNumber(t)) {
        result = AUTH_BAD_REQUEST;
    } else if (!cJSON_IsString(d) || strcmp(d->valuestring, util_device_id()) != 0) {
        result = AUTH_WRONG_DEVICE;
    } else {
        int64_t ts = (int64_t)t->valuedouble;
        int64_t now = util_wall_ms();
        if (ts <= key->last_t) {
            result = AUTH_REPLAY;
        } else if (now && (ts > now + MAX_CLOCK_SKEW_MS || ts < now - MAX_CLOCK_SKEW_MS)) {
            result = AUTH_CLOCK;
        } else {
            key->last_t = ts;
            save();
        }
    }
    if (result != AUTH_OK) {
        cJSON_Delete(p);
        return result;
    }
    *payload = p;
    return AUTH_OK;
}

auth_result_t keys_verify(const char *envelope, size_t len, cJSON **payload, key_info_t *who, char *re)
{
    *payload = NULL;
    re[0] = '\0';
    cJSON *env = cJSON_ParseWithLength(envelope, len);
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(env, "k");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(env, "p");
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(env, "s");
    uint32_t id;
    uint8_t sig[32];
    if (!cJSON_IsString(k) || !cJSON_IsString(p) || !cJSON_IsString(s) ||
        !parse_id(k->valuestring, &id) || !util_unhex(s->valuestring, sig, sizeof(sig))) {
        cJSON_Delete(env);
        return AUTH_BAD_REQUEST;
    }
    strlcpy(re, s->valuestring, 17);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stored_key_t *key = find(id);
    auth_result_t result = key ? check(key, p->valuestring, sig, payload) : AUTH_UNKNOWN_KEY;
    if (result == AUTH_OK) {
        to_info(key, who);
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(env);
    return result;
}

const char *keys_error(auth_result_t r)
{
    switch (r) {
    case AUTH_OK:
        return "ok";
    case AUTH_UNKNOWN_KEY:
        return "unknown_key";
    case AUTH_BAD_SIGNATURE:
        return "bad_signature";
    case AUTH_WRONG_DEVICE:
        return "wrong_device";
    case AUTH_REPLAY:
        return "replay";
    case AUTH_CLOCK:
        return "clock";
    default:
        return "bad_request";
    }
}

esp_err_t keys_hmac(uint32_t id, const void *msg, size_t len, uint8_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stored_key_t *key = find(id);
    if (key) {
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key->secret, sizeof(key->secret), msg, len,
                        out);
    }
    xSemaphoreGive(s_lock);
    return key ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// Caller holds s_lock.
static esp_err_t add_locked(const char *name, bool admin, key_info_t *out, char *secret_hex)
{
    if (s_db.count >= KEYS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    stored_key_t *k = &s_db.keys[s_db.count];
    memset(k, 0, sizeof(*k));
    do {
        k->id = esp_random();
    } while (k->id == 0 || find(k->id));   // find() doesn't see k yet: count isn't bumped
    strlcpy(k->name, name && name[0] ? name : "Phone", sizeof(k->name));
    k->admin = admin;
    esp_fill_random(k->secret, sizeof(k->secret));
    s_db.count++;
    save();
    to_info(k, out);
    util_hex(k->secret, sizeof(k->secret), secret_hex);
    return ESP_OK;
}

esp_err_t keys_add(const char *name, bool admin, key_info_t *out, char *secret_hex)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = add_locked(name, admin, out, secret_hex);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "added %s\"%s\"", out->admin ? "admin " : "", out->name);
        post_key_event(EV_KEY_ADD, out->name);
    }
    return err;
}

esp_err_t keys_remove(uint32_t id)
{
    char name[KEY_NAME_LEN] = "";
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stored_key_t *k = find(id);
    int admins = 0;
    for (uint32_t i = 0; i < s_db.count; i++) {
        admins += s_db.keys[i].admin;
    }
    if (!k) {
        err = ESP_ERR_NOT_FOUND;
    } else if (k->admin && admins == 1) {
        err = ESP_ERR_INVALID_STATE;   // nobody could manage the keys any more
    } else {
        strlcpy(name, k->name, sizeof(name));
        size_t index = k - s_db.keys;
        memmove(k, k + 1, (s_db.count - index - 1) * sizeof(*k));
        s_db.count--;
        memset(&s_db.keys[s_db.count], 0, sizeof(s_db.keys[0]));
        save();
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "removed \"%s\"", name);
        post_key_event(EV_KEY_REMOVE, name);
    }
    return err;
}

void keys_allow_claim(uint32_t window_ms)
{
    portENTER_CRITICAL(&s_claim_mux);
    s_claim_open = true;
    s_claim_until = util_uptime_ms() + window_ms;
    portEXIT_CRITICAL(&s_claim_mux);
    ESP_LOGI(TAG, "a phone can claim an admin key for the next %" PRIu32 " min", window_ms / 60000);
}

bool keys_claim_allowed(void)
{
    uint32_t now = util_uptime_ms();
    portENTER_CRITICAL(&s_claim_mux);
    bool open = s_claim_open && !util_due(now, s_claim_until);
    portEXIT_CRITICAL(&s_claim_mux);
    return open;
}

esp_err_t keys_claim(const char *name, key_info_t *out, char *secret_hex)
{
    uint32_t now = util_uptime_ms();
    portENTER_CRITICAL(&s_claim_mux);
    bool open = s_claim_open && !util_due(now, s_claim_until);
    s_claim_open = false;   // one phone per window
    portEXIT_CRITICAL(&s_claim_mux);
    if (!open) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = keys_add(name, true, out, secret_hex);
    if (err != ESP_OK) {
        keys_allow_claim(5 * 60 * 1000);   // the claim didn't happen; let them retry
    }
    return err;
}
