// Wi-Fi with a setup hotspot and captive portal. Started from the Mood Lamp's net.c.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "captive_dns.h"
#include "net.h"
#include "util.h"

#define RECONNECT_MIN_MS 1000
#define RECONNECT_MAX_MS 60000
#define SETUP_LINGER_MS 30000   // keep the hotspot up after joining so the page can say so

static const char *TAG = "net";

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_setup_timer;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;
static volatile bool s_local_disconnect;   // we called esp_wifi_disconnect() ourselves
static bool s_sntp_started;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static net_status_t s_status;              // guarded by s_mux

static void apply_tx_power(void)
{
#if CONFIG_DUMBLEDOOR_WIFI_MAX_TX_POWER >= 8
    esp_wifi_set_max_tx_power(CONFIG_DUMBLEDOOR_WIFI_MAX_TX_POWER);
#endif
}

static void schedule_reconnect(void)
{
    ESP_LOGI(TAG, "retrying in %" PRIu32 " s", s_backoff_ms / 1000);
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
    s_backoff_ms = MIN(s_backoff_ms * 2, RECONNECT_MAX_MS);
}

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

static void configure_ap(void)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, CONFIG_DUMBLEDOOR_SETUP_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    const char *password = CONFIG_DUMBLEDOOR_SETUP_PASSWORD;
    if (strlen(password) >= 8) {
        strlcpy((char *)cfg.ap.password, password, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else if (password[0]) {
        ESP_LOGW(TAG, "setup password is shorter than 8 characters, the hotspot will be open");
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
}

// RFC 8910: tells phones where the portal is, so the setup page opens by itself.
static void set_captive_portal_option(void)
{
    static char uri[32];   // the DHCP server keeps this pointer
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);
    snprintf(uri, sizeof(uri), "http://" IPSTR, IP2STR(&ip.ip));
    esp_netif_dhcps_stop(s_ap);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(s_ap);
}

static void stop_setup(void)
{
    portENTER_CRITICAL(&s_mux);
    bool was_active = s_status.setup_active;
    s_status.setup_active = false;
    portEXIT_CRITICAL(&s_mux);
    if (!was_active) {
        return;
    }
    ESP_LOGI(TAG, "closing setup hotspot");
    esp_timer_stop(s_setup_timer);
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    apply_tx_power();
}

static void setup_timer_cb(void *arg)
{
    stop_setup();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_status.has_credentials) {
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        bool local = e->reason == WIFI_REASON_ASSOC_LEAVE && s_local_disconnect;
        s_local_disconnect = false;
        portENTER_CRITICAL(&s_mux);
        bool had_ip = s_status.connected;
        s_status.connected = false;
        s_status.ip[0] = '\0';
        if (!local) {
            s_status.last_error = e->reason;
        }
        bool retry = s_status.has_credentials && !local;
        portEXIT_CRITICAL(&s_mux);
        if (local) {
            break;   // whoever disconnected reconnects
        }
        // The signal tells a weak link apart from a refused password (both can give reason 2).
        if (e->rssi < 0) {
            ESP_LOGW(TAG, "%s (reason %d, signal %d dBm)", had_ip ? "lost Wi-Fi" : "could not connect",
                     e->reason, e->rssi);
        } else {
            ESP_LOGW(TAG, "%s (reason %d)", had_ip ? "lost Wi-Fi" : "could not connect", e->reason);
        }
        if (retry) {
            schedule_reconnect();
        }
        break;
    }
    case WIFI_EVENT_AP_START:
        set_captive_portal_option();
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "a device joined the setup hotspot");
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *e = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_local_disconnect = false;
    s_backoff_ms = RECONNECT_MIN_MS;

    portENTER_CRITICAL(&s_mux);
    s_status.connected = true;
    s_status.last_error = 0;
    memcpy(s_status.ip, ip, sizeof(ip));
    bool setup = s_status.setup_active;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "connected: http://%s.local (http://%s)", CONFIG_DUMBLEDOOR_HOSTNAME, ip);
    if (setup) {
        esp_timer_stop(s_setup_timer);
        esp_timer_start_once(s_setup_timer, SETUP_LINGER_MS * 1000ULL);
    }
    if (!s_sntp_started) {
        // The clock lets the device reject old, replayed commands and timestamp the activity list.
        s_sntp_started = true;
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DUMBLEDOOR_NTP_SERVER);
        sntp.wait_for_sync = false;
        esp_netif_sntp_init(&sntp);
    }
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, CONFIG_DUMBLEDOOR_HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));

    const esp_timer_create_args_t reconnect = { .callback = reconnect_cb, .name = "wifi_retry" };
    const esp_timer_create_args_t setup = { .callback = setup_timer_cb, .name = "wifi_setup" };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_timer_create(&setup, &s_setup_timer));

    // Store the hotspot settings now so it comes up with the right name when opened.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap();

    wifi_config_t sta = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &sta);
    s_status.has_credentials = sta.sta.ssid[0] != '\0';
    memcpy(s_status.ssid, sta.sta.ssid, sizeof(sta.sta.ssid));   // not always NUL-terminated
    s_status.ssid[sizeof(sta.sta.ssid)] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(s_status.has_credentials ? WIFI_MODE_STA : WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_tx_power();

    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_init());
    mdns_hostname_set(CONFIG_DUMBLEDOOR_HOSTNAME);
    mdns_instance_name_set("Dumbledoor");
    mdns_txt_item_t txt[] = { { "id", util_device_id() } };
    mdns_service_add("Dumbledoor", "_http", "_tcp", 80, txt, 1);

    if (s_status.has_credentials) {
        ESP_LOGI(TAG, "joining \"%s\"", s_status.ssid);
    } else {
        ESP_LOGI(TAG, "no Wi-Fi saved yet");
        net_start_setup();
    }
}

void net_start_setup(void)
{
    if (!s_setup_timer) {
        return;   // net_start() hasn't run yet
    }
    esp_timer_stop(s_setup_timer);
    esp_timer_start_once(s_setup_timer, CONFIG_DUMBLEDOOR_SETUP_TIMEOUT_MIN * 60ULL * 1000000ULL);

    portENTER_CRITICAL(&s_mux);
    bool was_active = s_status.setup_active;
    s_status.setup_active = true;
    portEXIT_CRITICAL(&s_mux);
    if (was_active) {
        return;   // already open; the timeout was just extended
    }
    ESP_LOGI(TAG, "opening setup hotspot \"%s\"", CONFIG_DUMBLEDOOR_SETUP_SSID);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    apply_tx_power();
    captive_dns_start(s_ap);
}

bool net_setup_active(void)
{
    return s_status.setup_active;
}

bool net_connected(void)
{
    return s_status.connected;
}

void net_get_status(net_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_status.connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

bool net_is_setup_ip(uint32_t ip)
{
    esp_netif_ip_info_t info;
    return s_ap && esp_netif_get_ip_info(s_ap, &info) == ESP_OK && info.ip.addr == ip;
}

esp_err_t net_join(const char *ssid, const char *password)
{
    wifi_config_t cfg = { 0 };
    size_t ssid_len = strnlen(ssid, sizeof(cfg.sta.ssid) + 1);
    size_t pass_len = strnlen(password, sizeof(cfg.sta.password) + 1);
    if (ssid_len == 0 || ssid_len > sizeof(cfg.sta.ssid) || pass_len > sizeof(cfg.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    memcpy(cfg.sta.password, password, pass_len);
    cfg.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&s_mux);
    s_status.has_credentials = true;
    s_status.last_error = 0;
    memcpy(s_status.ssid, cfg.sta.ssid, sizeof(cfg.sta.ssid));
    s_status.ssid[sizeof(cfg.sta.ssid)] = '\0';
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "joining \"%.*s\"", (int)ssid_len, ssid);
    esp_timer_stop(s_reconnect_timer);
    s_backoff_ms = RECONNECT_MIN_MS;
    s_local_disconnect = true;
    esp_wifi_disconnect();
    return esp_wifi_connect();
}

int net_scan(wifi_ap_record_t *records, int max)
{
    wifi_scan_config_t cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err == ESP_ERR_WIFI_STATE) {
        // The station is busy (re)connecting; pause it for the scan.
        esp_timer_stop(s_reconnect_timer);
        s_local_disconnect = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(&cfg, true);
        net_status_t st;
        net_get_status(&st);
        if (st.has_credentials && !st.connected) {
            schedule_reconnect();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t n = max;
    esp_wifi_scan_get_ap_records(&n, records);
    return n;
}
