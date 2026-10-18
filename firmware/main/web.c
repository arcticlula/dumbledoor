#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "api.h"
#include "keys.h"
#include "net.h"
#include "settings.h"
#include "util.h"
#include "web.h"

#define MAX_BODY 2048
#define MAX_SCAN 20
#define OTA_CHUNK 2048

static const char *TAG = "web";

// The app's files, built into the firmware by main/CMakeLists.txt.
#define ASSET(sym)                                                     \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[] asm("_binary_" #sym "_end")

ASSET(index_html);
ASSET(style_css);
ASSET(main_js);
ASSET(device_js);
ASSET(crypto_js);
ASSET(mqtt_js);
ASSET(sw_js);
ASSET(manifest_webmanifest);
ASSET(icon_svg);
ASSET(icon_180_png);
ASSET(icon_192_png);
ASSET(icon_512_png);
ASSET(icon_maskable_png);

typedef struct {
    const char *uri;
    const char *type;
    const uint8_t *start;
    const uint8_t *end;
    bool text;   // EMBED_TXTFILES adds a NUL that isn't part of the file
} asset_t;

static const asset_t s_assets[] = {
    { "/", "text/html; charset=utf-8", index_html_start, index_html_end, true },
    { "/index.html", "text/html; charset=utf-8", index_html_start, index_html_end, true },
    { "/style.css", "text/css; charset=utf-8", style_css_start, style_css_end, true },
    { "/main.js", "text/javascript; charset=utf-8", main_js_start, main_js_end, true },
    { "/device.js", "text/javascript; charset=utf-8", device_js_start, device_js_end, true },
    { "/crypto.js", "text/javascript; charset=utf-8", crypto_js_start, crypto_js_end, true },
    { "/mqtt.js", "text/javascript; charset=utf-8", mqtt_js_start, mqtt_js_end, true },
    { "/sw.js", "text/javascript; charset=utf-8", sw_js_start, sw_js_end, true },
    { "/manifest.webmanifest", "application/manifest+json", manifest_webmanifest_start, manifest_webmanifest_end,
      true },
    { "/icon.svg", "image/svg+xml", icon_svg_start, icon_svg_end, true },
    { "/icon-180.png", "image/png", icon_180_png_start, icon_180_png_end, false },
    { "/icon-192.png", "image/png", icon_192_png_start, icon_192_png_end, false },
    { "/icon-512.png", "image/png", icon_512_png_start, icon_512_png_end, false },
    { "/icon-maskable.png", "image/png", icon_maskable_png_start, icon_maskable_png_end, false },
};

// A WebSocket page that has signed in with a valid key.
typedef struct {
    uint32_t key_id;
} ws_session_t;

static httpd_handle_t s_server;
static atomic_bool s_broadcast_queued;
static char s_etag[16];   // changes with every build, so phones pick up a new app

static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    return buf;
}

// Sends `json` (freed here), or {"ok":true} if it's NULL.
static esp_err_t send_json(httpd_req_t *req, char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{\"ok\":true}");
    free(json);
    return err;
}

// Sends `o` and deletes it.
static esp_err_t send_cjson(httpd_req_t *req, cJSON *o)
{
    char *json = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return send_json(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *error)
{
    char json[80];
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

// Whether the request came in over the setup hotspot rather than the home network.
static bool via_setup(httpd_req_t *req)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        return false;
    }
    if (addr.ss_family == AF_INET) {
        return net_is_setup_ip(((struct sockaddr_in *)&addr)->sin_addr.s_addr);
    }
#if CONFIG_LWIP_IPV6
    if (addr.ss_family == AF_INET6) {
        // The server's sockets are dual-stack: IPv4 arrives as ::ffff:a.b.c.d.
        return net_is_setup_ip(((struct sockaddr_in6 *)&addr)->sin6_addr.un.u32_addr[3]);
    }
#endif
    return false;
}

static esp_err_t asset_get(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;
    char tag[sizeof(s_etag)];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", tag, sizeof(tag)) == ESP_OK && !strcmp(tag, s_etag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", s_etag);
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", s_etag);
    return httpd_resp_send(req, (const char *)a->start, a->end - a->start - (a->text ? 1 : 0));
}

// Without this, browsers' icon requests would be redirected and download the whole page again.
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// Public: what the app needs before it has a key.
static esp_err_t info_get(httpd_req_t *req)
{
    char name[32];
    settings_get_name(name, sizeof(name));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", API_VERSION);
    cJSON_AddStringToObject(o, "id", util_device_id());
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "host", CONFIG_DUMBLEDOOR_HOSTNAME ".local");
    cJSON_AddBoolToObject(o, "setup", net_setup_active());
    cJSON_AddBoolToObject(o, "viaSetup", via_setup(req));
    cJSON_AddBoolToObject(o, "claim", keys_claim_allowed());
    cJSON_AddBoolToObject(o, "paired", keys_count() > 0);
    return send_cjson(req, o);
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    net_status_t st;
    net_get_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "setup", st.setup_active);
    cJSON_AddBoolToObject(o, "configured", st.has_credentials);
    cJSON_AddBoolToObject(o, "connected", st.connected);
    cJSON_AddStringToObject(o, "ssid", st.ssid);
    cJSON_AddStringToObject(o, "ip", st.ip);
    cJSON_AddStringToObject(o, "host", CONFIG_DUMBLEDOOR_HOSTNAME ".local");
    cJSON_AddStringToObject(o, "setupSsid", CONFIG_DUMBLEDOOR_SETUP_SSID);
    cJSON_AddNumberToObject(o, "error", st.last_error);
    return send_cjson(req, o);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!net_setup_active()) {
        return send_error(req, "403 Forbidden", "setup_closed");
    }
    char *body = read_body(req);
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(o, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(o, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid)) {
        err = net_join(ssid->valuestring, cJSON_IsString(password) ? password->valuestring : "");
    }
    cJSON_Delete(o);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid");
    }
    return send_json(req, NULL);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!net_setup_active()) {
        return send_error(req, "403 Forbidden", "setup_closed");
    }
    wifi_ap_record_t *aps = calloc(MAX_SCAN, sizeof(*aps));
    if (!aps) {
        return httpd_resp_send_500(req);
    }
    int n = net_scan(aps, MAX_SCAN);
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        bool duplicate = ssid[0] == '\0';   // also skips hidden networks
        for (int j = 0; j < i && !duplicate; j++) {
            duplicate = strcmp(ssid, (const char *)aps[j].ssid) == 0;   // mesh nodes share a name
        }
        if (duplicate) {
            continue;
        }
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(ap, "open", aps[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list, ap);
    }
    free(aps);
    return send_cjson(req, list);
}

// The first admin key, or a replacement after the setup button was held. See keys_claim().
static esp_err_t claim_post(httpd_req_t *req)
{
    char *body = read_body(req);
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
    char key_name[KEY_NAME_LEN] = "Phone";
    if (cJSON_IsString(name) && name->valuestring[0]) {
        strlcpy(key_name, name->valuestring, sizeof(key_name));
    }
    cJSON_Delete(o);

    key_info_t key;
    char secret[65];
    esp_err_t err = keys_claim(key_name, &key, secret);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "403 Forbidden", "claim_closed");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "full");
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON *pairing = api_pairing(&key, secret);
    memset(secret, 0, sizeof(secret));
    if (pairing) {
        cJSON_AddItemToObject(reply, "pair", pairing);
    }
    return send_cjson(req, reply);
}

// A signed command over plain HTTP, for scripts. The app uses the WebSocket.
static esp_err_t cmd_post(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_error(req, "400 Bad Request", "bad_request");
    }
    key_info_t who;
    bool authed;
    cJSON *reply = api_run(body, strlen(body), API_LOCAL, &who, &authed);
    free(body);
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok"))) {
        httpd_resp_set_status(req, authed ? "400 Bad Request" : "401 Unauthorized");
    }
    return send_cjson(req, reply);
}

// New firmware as the request body, signed {"a":"ota"} command from an admin key in X-Auth.
static esp_err_t ota_post(httpd_req_t *req)
{
    size_t auth_len = httpd_req_get_hdr_value_len(req, "X-Auth");
    char *auth = auth_len > 0 && auth_len <= MAX_BODY ? malloc(auth_len + 1) : NULL;
    if (!auth || httpd_req_get_hdr_value_str(req, "X-Auth", auth, auth_len + 1) != ESP_OK) {
        free(auth);
        return send_error(req, "401 Unauthorized", "bad_request");
    }
    cJSON *payload = NULL;
    key_info_t who;
    char re[17];
    auth_result_t r = keys_verify(auth, auth_len, &payload, &who, re);
    free(auth);
    if (r != AUTH_OK) {
        return send_error(req, "401 Unauthorized", keys_error(r));
    }
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(payload, "a");
    bool is_ota = cJSON_IsString(a) && !strcmp(a->valuestring, "ota");
    cJSON_Delete(payload);
    if (!is_ota) {
        return send_error(req, "400 Bad Request", "unknown_action");
    }
    if (!who.admin) {
        return send_error(req, "403 Forbidden", "forbidden");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return send_error(req, "400 Bad Request", "bad_size");
    }

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "ota_failed");
    }
    ESP_LOGI(TAG, "receiving %u bytes of firmware from %s", (unsigned)req->content_len, who.name);
    char *buf = malloc(OTA_CHUNK);
    err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    size_t got = 0;
    while (err == ESP_OK && got < req->content_len) {
        int n = httpd_req_recv(req, buf, MIN(OTA_CHUNK, req->content_len - got));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, n);
        got += n;
    }
    free(buf);
    if (err == ESP_OK) {
        err = esp_ota_end(ota);   // checks the image; frees the handle either way
    } else {
        esp_ota_abort(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firmware update failed: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request", err == ESP_ERR_OTA_VALIDATE_FAILED ? "invalid_image" : "ota_failed");
    }
    ESP_LOGI(TAG, "firmware updated, restarting");
    util_restart_soon(1000);
    return send_json(req, NULL);
}

static void sign_in(httpd_req_t *req, uint32_t key_id)
{
    int fd = httpd_req_to_sockfd(req);
    ws_session_t *s = httpd_sess_get_ctx(req->handle, fd);
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s) {
            return;
        }
        httpd_sess_set_ctx(req->handle, fd, s, free);
    }
    s->key_id = key_id;
}

// The app's connection on the home network: signed commands in, replies and state out.
// A page gets state pushes once one of its commands has passed the signature check.
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK;   // handshake done; the page signs in with its first message
    }
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);   // length only
    if (err != ESP_OK || frame.len == 0) {
        return err;
    }
    if (frame.len > MAX_BODY) {
        return ESP_FAIL;   // closes the connection
    }
    uint8_t *buf = malloc(frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        buf[frame.len] = '\0';
        key_info_t who;
        bool authed;
        cJSON *reply = api_run((const char *)buf, frame.len, API_LOCAL, &who, &authed);
        if (authed) {
            sign_in(req, who.id);
        }
        char *json = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        if (json) {
            httpd_ws_frame_t out = {
                .final = true,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)json,
                .len = strlen(json),
            };
            err = httpd_ws_send_frame(req, &out);
            free(json);
        }
    }
    free(buf);
    return err;
}

// Phones probe a known URL to detect captive portals; sending every unknown page
// to the app makes the setup page pop up. iOS also needs a body.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t error)
{
    if (!strncmp(req->uri, "/api/", 5)) {
        return send_error(req, "404 Not Found", "not_found");
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Dumbledoor");
    return ESP_OK;
}

static void broadcast(void *arg)
{
    atomic_store(&s_broadcast_queued, false);
    cJSON *state = api_state();
    char *json = cJSON_PrintUnformatted(state);
    cJSON_Delete(state);
    if (!json) {
        return;
    }
    size_t count = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(s_server, &count, fds) == ESP_OK) {
        httpd_ws_frame_t frame = {
            .final = true,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json),
        };
        for (size_t i = 0; i < count; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
                continue;
            }
            const ws_session_t *s = httpd_sess_get_ctx(s_server, fds[i]);
            if (!s) {
                continue;   // hasn't signed in
            }
            if (!keys_exists(s->key_id)) {
                httpd_sess_trigger_close(s_server, fds[i]);   // its key was removed
                continue;
            }
            httpd_ws_send_frame_async(s_server, fds[i], &frame);
        }
    }
    free(json);
}

void web_notify(void)
{
    // Changes that arrive while a broadcast is queued go out with it.
    if (!s_server || atomic_exchange(&s_broadcast_queued, true)) {
        return;
    }
    if (httpd_queue_work(s_server, broadcast, NULL) != ESP_OK) {
        atomic_store(&s_broadcast_queued, false);
    }
}

void web_start(void)
{
    // Captive-portal probes produce a stream of harmless warnings.
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    snprintf(s_etag, sizeof(s_etag), "\"%s\"", sha);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 8;
    cfg.max_uri_handlers = 32;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "couldn't start the web server");
        s_server = NULL;
        return;
    }

    for (size_t i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); i++) {
        const httpd_uri_t route = {
            .uri = s_assets[i].uri,
            .method = HTTP_GET,
            .handler = asset_get,
            .user_ctx = (void *)&s_assets[i],
        };
        httpd_register_uri_handler(s_server, &route);
    }
    static const httpd_uri_t routes[] = {
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get },
        { .uri = "/api/info", .method = HTTP_GET, .handler = info_get },
        { .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get },
        { .uri = "/api/claim", .method = HTTP_POST, .handler = claim_post },
        { .uri = "/api/cmd", .method = HTTP_POST, .handler = cmd_post },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post },
        { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
}
