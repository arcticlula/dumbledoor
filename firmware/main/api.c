#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "api.h"
#include "door.h"
#include "events.h"
#include "net.h"
#include "remote.h"
#include "settings.h"
#include "util.h"

#define AUTO_MAX_MIN (12 * 60)
#define MUTE_MAX_MIN (24 * 60)
#define DENIED_LOG_GAP_MS 10000   // list at most one rejected command per 10 s, so junk can't flood it
#define LOCAL_APP_URL "http://" CONFIG_DUMBLEDOOR_HOSTNAME ".local/"

static const char *TAG = "api";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_denied_before;   // guarded by s_mux
static uint32_t s_denied_ms;

static cJSON *event_json(const event_t *e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", e->n);
    cJSON_AddNumberToObject(o, "time", (double)e->time_ms);
    cJSON_AddNumberToObject(o, "up", e->up_ms);
    cJSON_AddStringToObject(o, "e", events_type_name(e->type));
    switch (e->type) {
    case EV_BOOT:
        cJSON_AddStringToObject(o, "reason", e->who);
        break;
    case EV_RING:
        cJSON_AddStringToObject(o, "line", door_line_name(e->line));
        cJSON_AddNumberToObject(o, "ms", e->value);
        break;
    case EV_OPEN:
        cJSON_AddStringToObject(o, "by", e->who);
        cJSON_AddStringToObject(o, "via", e->via);
        break;
    case EV_AUTO:
        cJSON_AddStringToObject(o, "by", e->who);
        cJSON_AddNumberToObject(o, "min", e->value);
        break;
    case EV_MUTE:
        cJSON_AddStringToObject(o, "by", e->who);
        cJSON_AddNumberToObject(o, "min", e->value == UINT32_MAX ? -1 : (double)e->value);
        break;
    case EV_CODE_FAIL:
        cJSON_AddNumberToObject(o, "count", e->value);
        break;
    case EV_DENIED:
        cJSON_AddStringToObject(o, "why", e->who);
        cJSON_AddStringToObject(o, "via", e->via);
        break;
    case EV_KEY_ADD:
    case EV_KEY_REMOVE:
        cJSON_AddStringToObject(o, "name", e->who);
        break;
    default:
        break;
    }
    return o;
}

cJSON *api_state(void)
{
    door_state_t door;
    door_get_state(&door);
    char name[32];
    settings_get_name(name, sizeof(name));
    net_status_t net;
    net_get_status(&net);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "state");
    cJSON_AddNumberToObject(o, "v", API_VERSION);
    cJSON_AddStringToObject(o, "id", util_device_id());
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "time", (double)util_wall_ms());
    cJSON_AddNumberToObject(o, "up", util_uptime_ms());
    cJSON_AddBoolToObject(o, "pins", door.outputs_ok);

    cJSON *lines = cJSON_AddArrayToObject(o, "lines");
    cJSON *ringing = cJSON_AddObjectToObject(o, "ringing");
    for (int i = 0; i < LINE_COUNT; i++) {
        if (door.has_line[i]) {
            cJSON_AddItemToArray(lines, cJSON_CreateString(door_line_name(i)));
            cJSON_AddBoolToObject(ringing, door_line_name(i), door.ringing[i]);
        }
    }
    cJSON_AddBoolToObject(o, "opening", door.opening);
    cJSON_AddNumberToObject(o, "auto", door.auto_left_s);
    cJSON_AddBoolToObject(o, "bell", door.has_bell);
    cJSON_AddNumberToObject(o, "mute", door.mute_left_s);
    cJSON *code = cJSON_AddObjectToObject(o, "code");
    cJSON_AddBoolToObject(code, "on", door.code_on);
    cJSON_AddNumberToObject(code, "locked", door.code_locked_s);
    cJSON *remote = cJSON_AddObjectToObject(o, "remote");
    cJSON_AddBoolToObject(remote, "on", remote_enabled());
    cJSON_AddBoolToObject(remote, "ok", remote_connected());
    cJSON *wifi = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", net.ssid);
    cJSON_AddNumberToObject(wifi, "rssi", net_rssi());
    cJSON_AddNumberToObject(o, "heap", esp_get_free_heap_size());

    cJSON *list = cJSON_AddArrayToObject(o, "events");
    event_t *events = malloc(EVENTS_KEEP * sizeof(event_t));
    if (events) {
        int n = events_recent(events, EVENTS_KEEP);
        for (int i = 0; i < n; i++) {
            cJSON_AddItemToArray(list, event_json(&events[i]));
        }
        free(events);
    }
    return o;
}

cJSON *api_pairing(const key_info_t *key, const char *secret_hex)
{
    dd_settings_t *s = malloc(sizeof(*s));
    if (!s) {
        return NULL;
    }
    settings_get(s);
    char id[9];
    snprintf(id, sizeof(id), "%08" PRIx32, key->id);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", API_VERSION);
    cJSON_AddStringToObject(o, "d", util_device_id());
    cJSON_AddStringToObject(o, "dn", s->name);
    cJSON_AddStringToObject(o, "k", id);
    cJSON_AddStringToObject(o, "s", secret_hex);
    cJSON_AddStringToObject(o, "n", key->name);
    cJSON_AddBoolToObject(o, "a", key->admin);
    if (s->mqtt_on && s->app_mqtt_url[0]) {
        // Phones use their own broker login if there is one, else the device's.
        bool own = s->app_mqtt_user[0] != '\0';
        cJSON *m = cJSON_AddObjectToObject(o, "m");
        cJSON_AddStringToObject(m, "u", s->app_mqtt_url);
        cJSON_AddStringToObject(m, "user", own ? s->app_mqtt_user : s->mqtt_user);
        cJSON_AddStringToObject(m, "pass", own ? s->app_mqtt_pass : s->mqtt_pass);
        cJSON_AddStringToObject(m, "t", s->mqtt_prefix);
    }
    free(s);
    return o;
}

// "<base>#pair=<base64url of the pairing JSON>". Caller frees.
static char *pairing_link(const char *base, const cJSON *pairing)
{
    char *json = cJSON_PrintUnformatted(pairing);
    if (!json) {
        return NULL;
    }
    size_t len = strlen(json);
    size_t b64_size = 4 * ((len + 2) / 3) + 1;
    size_t base_len = strlen(base);
    char *link = malloc(base_len + 6 + b64_size);
    if (link) {
        memcpy(link, base, base_len);
        memcpy(link + base_len, "#pair=", 6);
        unsigned char *out = (unsigned char *)link + base_len + 6;
        size_t n = 0;
        mbedtls_base64_encode(out, b64_size, &n, (const unsigned char *)json, len);
        // base64url without padding, so the link survives being pasted into chat apps
        for (size_t i = 0; i < n; i++) {
            if (out[i] == '+') {
                out[i] = '-';
            } else if (out[i] == '/') {
                out[i] = '_';
            }
        }
        while (n > 0 && out[n - 1] == '=') {
            n--;
        }
        out[n] = '\0';
    }
    free(json);
    return link;
}

static void log_denied(auth_result_t r, api_via_t via)
{
    uint32_t now = util_uptime_ms();
    portENTER_CRITICAL(&s_mux);
    bool quiet = s_denied_before && now - s_denied_ms < DENIED_LOG_GAP_MS;
    if (!quiet) {
        s_denied_before = true;
        s_denied_ms = now;
    }
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "rejected a %s command: %s", via == API_LOCAL ? "local" : "remote", keys_error(r));
    if (!quiet) {
        event_t e = { .type = EV_DENIED };
        strlcpy(e.who, keys_error(r), sizeof(e.who));
        strlcpy(e.via, via == API_LOCAL ? "local" : "remote", sizeof(e.via));
        events_post(&e);
    }
}

// Settings ------------------------------------------------------------------

static cJSON *settings_json(const dd_settings_t *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", s->name);
    cJSON *code = cJSON_AddObjectToObject(o, "code");
    cJSON_AddBoolToObject(code, "on", s->code.on);
    cJSON_AddStringToObject(code, "seq", s->code.seq);
    cJSON_AddNumberToObject(code, "short", s->code.short_ms);
    cJSON_AddNumberToObject(code, "long", s->code.long_ms);
    cJSON_AddNumberToObject(code, "gap", s->code.gap_ms);
    // Passwords never leave the device; the app only learns whether one is set.
    cJSON *mqtt = cJSON_AddObjectToObject(o, "mqtt");
    cJSON_AddBoolToObject(mqtt, "on", s->mqtt_on);
    cJSON_AddStringToObject(mqtt, "url", s->mqtt_url);
    cJSON_AddStringToObject(mqtt, "user", s->mqtt_user);
    cJSON_AddBoolToObject(mqtt, "hasPass", s->mqtt_pass[0] != '\0');
    cJSON_AddStringToObject(mqtt, "prefix", s->mqtt_prefix);
    cJSON_AddStringToObject(mqtt, "appUrl", s->app_mqtt_url);
    cJSON_AddStringToObject(mqtt, "appUser", s->app_mqtt_user);
    cJSON_AddBoolToObject(mqtt, "hasAppPass", s->app_mqtt_pass[0] != '\0');
    cJSON_AddStringToObject(o, "appUrl", s->app_url);
    cJSON_AddStringToObject(o, "ntfy", s->ntfy_url);
    return o;
}

// The take_* helpers copy o[key] into dst when it's there, and return false when it's
// there but the wrong type or out of range.

static bool take_string(const cJSON *o, const char *key, char *dst, size_t size)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsString(it) || strlen(it->valuestring) >= size) {
        return false;
    }
    strlcpy(dst, it->valuestring, size);
    return true;
}

// The app never sees passwords, so an empty one means "keep the current one".
static bool take_secret(const cJSON *o, const char *key, char *dst, size_t size)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(it) && !it->valuestring[0]) {
        return true;
    }
    return take_string(o, key, dst, size);
}

static bool take_bool(const cJSON *o, const char *key, bool *dst)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsBool(it)) {
        return false;
    }
    *dst = cJSON_IsTrue(it);
    return true;
}

static bool take_number(const cJSON *o, const char *key, int lo, int hi, uint16_t *dst)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsNumber(it) || it->valuedouble < lo || it->valuedouble > hi) {
        return false;
    }
    *dst = (uint16_t)it->valuedouble;
    return true;
}

static bool has_scheme(const char *url, const char *a, const char *b)
{
    return !strncmp(url, a, strlen(a)) || (b && !strncmp(url, b, strlen(b)));
}

static const char *apply_settings(dd_settings_t *s, const cJSON *set)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(set, "code");
    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(set, "mqtt");
    bool ok = take_string(set, "name", s->name, sizeof(s->name)) &&
              take_string(set, "appUrl", s->app_url, sizeof(s->app_url)) &&
              take_string(set, "ntfy", s->ntfy_url, sizeof(s->ntfy_url));
    if (ok && code) {
        ok = cJSON_IsObject(code) && take_bool(code, "on", &s->code.on) &&
             take_string(code, "seq", s->code.seq, sizeof(s->code.seq)) &&
             take_number(code, "short", 100, 5000, &s->code.short_ms) &&
             take_number(code, "long", 100, 5000, &s->code.long_ms) &&
             take_number(code, "gap", 300, 10000, &s->code.gap_ms);
    }
    if (ok && mqtt) {
        ok = cJSON_IsObject(mqtt) && take_bool(mqtt, "on", &s->mqtt_on) &&
             take_string(mqtt, "url", s->mqtt_url, sizeof(s->mqtt_url)) &&
             take_string(mqtt, "user", s->mqtt_user, sizeof(s->mqtt_user)) &&
             take_secret(mqtt, "pass", s->mqtt_pass, sizeof(s->mqtt_pass)) &&
             take_string(mqtt, "prefix", s->mqtt_prefix, sizeof(s->mqtt_prefix)) &&
             take_string(mqtt, "appUrl", s->app_mqtt_url, sizeof(s->app_mqtt_url)) &&
             take_string(mqtt, "appUser", s->app_mqtt_user, sizeof(s->app_mqtt_user)) &&
             take_secret(mqtt, "appPass", s->app_mqtt_pass, sizeof(s->app_mqtt_pass));
    }
    if (!ok || !s->name[0]) {
        return "invalid";
    }
    if (strspn(s->code.seq, ".-") != strlen(s->code.seq) || (s->code.on && !s->code.seq[0]) ||
        s->code.short_ms > s->code.long_ms) {
        return "invalid_code";
    }
    if ((s->mqtt_url[0] && !has_scheme(s->mqtt_url, "mqtt://", "mqtts://") &&
         !has_scheme(s->mqtt_url, "ws://", "wss://")) ||
        (s->mqtt_on && !s->mqtt_url[0]) ||
        (s->app_mqtt_url[0] && !has_scheme(s->app_mqtt_url, "wss://", "ws://"))) {
        return "invalid_mqtt";
    }
    if (strpbrk(s->mqtt_prefix, "+#") || s->mqtt_prefix[0] == '/') {
        return "invalid_mqtt";
    }
    if (!s->mqtt_prefix[0]) {
        strlcpy(s->mqtt_prefix, "dumbledoor", sizeof(s->mqtt_prefix));
    }
    if ((s->app_url[0] && !has_scheme(s->app_url, "https://", "http://")) ||
        (s->ntfy_url[0] && !has_scheme(s->ntfy_url, "https://", "http://"))) {
        return "invalid_url";
    }
    return NULL;
}

static bool mqtt_changed(const dd_settings_t *a, const dd_settings_t *b)
{
    return a->mqtt_on != b->mqtt_on || strcmp(a->mqtt_url, b->mqtt_url) || strcmp(a->mqtt_user, b->mqtt_user) ||
           strcmp(a->mqtt_pass, b->mqtt_pass) || strcmp(a->mqtt_prefix, b->mqtt_prefix);
}

static const char *run_settings(const cJSON *p, cJSON *reply)
{
    dd_settings_t *old = malloc(sizeof(*old));
    dd_settings_t *s = malloc(sizeof(*s));
    const char *error = NULL;
    if (!old || !s) {
        error = "no_memory";
    } else {
        settings_get(old);
        *s = *old;
        const cJSON *set = cJSON_GetObjectItemCaseSensitive(p, "set");
        if (cJSON_IsObject(set)) {
            error = apply_settings(s, set);
            if (!error) {
                settings_set(s);
                if (mqtt_changed(old, s)) {
                    remote_restart();
                }
            }
        }
        if (!error) {
            cJSON_AddItemToObject(reply, "settings", settings_json(s));
        }
    }
    free(old);
    free(s);
    return error;
}

// Keys ----------------------------------------------------------------------

static const char *run_keys(cJSON *reply)
{
    key_info_t *keys = malloc(KEYS_MAX * sizeof(key_info_t));
    if (!keys) {
        return "no_memory";
    }
    int n = keys_list(keys, KEYS_MAX);
    cJSON *list = cJSON_AddArrayToObject(reply, "keys");
    for (int i = 0; i < n; i++) {
        char id[9];
        snprintf(id, sizeof(id), "%08" PRIx32, keys[i].id);
        cJSON *k = cJSON_CreateObject();
        cJSON_AddStringToObject(k, "id", id);
        cJSON_AddStringToObject(k, "name", keys[i].name);
        cJSON_AddBoolToObject(k, "admin", keys[i].admin);
        cJSON_AddNumberToObject(k, "last", (double)keys[i].last_t);
        cJSON_AddItemToArray(list, k);
    }
    free(keys);
    return NULL;
}

static const char *run_key_add(const cJSON *p, cJSON *reply)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
    const cJSON *admin = cJSON_GetObjectItemCaseSensitive(p, "admin");
    if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) >= KEY_NAME_LEN) {
        return "invalid";
    }
    key_info_t key;
    char secret[65];
    esp_err_t err = keys_add(name->valuestring, cJSON_IsTrue(admin), &key, secret);
    if (err != ESP_OK) {
        return err == ESP_ERR_NO_MEM ? "full" : "failed";
    }
    cJSON *pairing = api_pairing(&key, secret);
    memset(secret, 0, sizeof(secret));
    if (!pairing) {
        return "no_memory";
    }
    char *link = NULL;
    dd_settings_t *s = malloc(sizeof(*s));
    if (s) {
        settings_get(s);
        if (s->app_url[0]) {
            link = pairing_link(s->app_url, pairing);
        }
        free(s);
    }
    char *local = pairing_link(LOCAL_APP_URL, pairing);
    if (link) {
        cJSON_AddStringToObject(reply, "link", link);
    }
    if (local) {
        cJSON_AddStringToObject(reply, "localLink", local);
    }
    free(link);
    free(local);
    cJSON_AddItemToObject(reply, "pair", pairing);
    return NULL;
}

static const char *run_key_remove(const cJSON *p)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(p, "id");
    uint8_t b[4];
    if (!cJSON_IsString(id) || !util_unhex(id->valuestring, b, sizeof(b))) {
        return "invalid";
    }
    esp_err_t err = keys_remove((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
    if (err == ESP_ERR_NOT_FOUND) {
        return "not_found";
    }
    return err == ESP_ERR_INVALID_STATE ? "last_admin" : NULL;
}

// Commands ------------------------------------------------------------------

static const char *run_action(const char *a, const cJSON *p, api_via_t via, const key_info_t *who, cJSON *reply)
{
    if (!strcmp(a, "hello") || !strcmp(a, "state")) {
        cJSON_AddStringToObject(reply, "name", who->name);
        cJSON_AddBoolToObject(reply, "admin", who->admin);
        cJSON_AddItemToObject(reply, "state", api_state());
        return NULL;
    }
    if (!strcmp(a, "open")) {
        esp_err_t err = door_open(who->name, via == API_LOCAL ? "local" : "remote");
        return err == ESP_OK ? NULL : err == ESP_ERR_NOT_SUPPORTED ? "no_pins" : "busy";
    }
    if (!strcmp(a, "auto")) {
        const cJSON *min = cJSON_GetObjectItemCaseSensitive(p, "min");
        if (!cJSON_IsNumber(min) || min->valuedouble < 0 || min->valuedouble > AUTO_MAX_MIN) {
            return "invalid";
        }
        door_set_auto((uint32_t)min->valuedouble, who->name);
        return NULL;
    }
    if (!strcmp(a, "mute")) {
        const cJSON *min = cJSON_GetObjectItemCaseSensitive(p, "min");
        if (!cJSON_IsNumber(min) || min->valuedouble < -1 || min->valuedouble > MUTE_MAX_MIN) {
            return "invalid";
        }
        door_set_mute((int32_t)min->valuedouble, who->name);
        return NULL;
    }

    // The rest decides who can get in, so it takes an admin key on the home network.
    bool admin_action = !strcmp(a, "settings") || !strcmp(a, "keys") || !strcmp(a, "key_add") ||
                        !strcmp(a, "key_remove") || !strcmp(a, "reboot");
    if (!admin_action) {
        return "unknown_action";
    }
    if (!who->admin) {
        return "forbidden";
    }
    if (via != API_LOCAL) {
        return "local_only";
    }
    if (!strcmp(a, "settings")) {
        return run_settings(p, reply);
    }
    if (!strcmp(a, "keys")) {
        return run_keys(reply);
    }
    if (!strcmp(a, "key_add")) {
        return run_key_add(p, reply);
    }
    if (!strcmp(a, "key_remove")) {
        return run_key_remove(p);
    }
    ESP_LOGI(TAG, "restarting, asked by %s", who->name);
    util_restart_soon(500);
    return NULL;
}

cJSON *api_run(const char *envelope, size_t len, api_via_t via, key_info_t *who, bool *authed)
{
    *authed = false;
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "reply");

    cJSON *payload = NULL;
    char re[17];
    auth_result_t r = keys_verify(envelope, len, &payload, who, re);
    if (re[0]) {
        cJSON_AddStringToObject(reply, "re", re);
    }
    const char *error;
    if (r != AUTH_OK) {
        if (r == AUTH_UNKNOWN_KEY || r == AUTH_BAD_SIGNATURE || r == AUTH_WRONG_DEVICE) {
            log_denied(r, via);
        }
        error = keys_error(r);
    } else {
        *authed = true;
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(payload, "a");
        error = run_action(cJSON_IsString(a) ? a->valuestring : "", payload, via, who, reply);
        cJSON_Delete(payload);
    }
    cJSON_AddBoolToObject(reply, "ok", error == NULL);
    if (error) {
        cJSON_AddStringToObject(reply, "error", error);
    }
    return reply;
}
