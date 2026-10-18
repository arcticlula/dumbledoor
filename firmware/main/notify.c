#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "net.h"
#include "notify.h"
#include "settings.h"
#include "util.h"

#define NOTIFY_GAP_MS 30000   // one notification per line per 30 s, however often they press

static const char *TAG = "notify";

static QueueHandle_t s_queue;
static bool s_sent[LINE_COUNT];   // only used by notify_task
static uint32_t s_sent_ms[LINE_COUNT];

static void post(const dd_settings_t *s, line_t line)
{
    const char *message = line == LINE_LOBBY ? "Someone is ringing at the lobby" : "Someone is at your front door";
    esp_http_client_config_t cfg = {
        .url = s->ntfy_url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "bad ntfy address");
        return;
    }
    esp_http_client_set_header(client, "Title", s->name);
    esp_http_client_set_header(client, "Tags", "bell");
    esp_http_client_set_header(client, "Priority", "high");
    if (s->app_url[0]) {
        esp_http_client_set_header(client, "Click", s->app_url);   // tapping it opens the app
    }
    esp_http_client_set_post_field(client, message, strlen(message));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK || status >= 300) {
        ESP_LOGW(TAG, "notification failed: %s, HTTP %d", esp_err_to_name(err), status);
    }
    esp_http_client_cleanup(client);
}

static void notify_task(void *arg)
{
    uint8_t line;
    for (;;) {
        xQueueReceive(s_queue, &line, portMAX_DELAY);
        uint32_t now = util_uptime_ms();
        if (line >= LINE_COUNT || (s_sent[line] && now - s_sent_ms[line] < NOTIFY_GAP_MS) || !net_connected()) {
            continue;
        }
        dd_settings_t *s = malloc(sizeof(*s));
        if (!s) {
            continue;
        }
        settings_get(s);
        if (s->ntfy_url[0]) {
            s_sent[line] = true;
            s_sent_ms[line] = now;
            post(s, line);
        }
        free(s);
    }
}

void notify_start(void)
{
    s_queue = xQueueCreate(4, sizeof(uint8_t));
    // TLS needs a deep stack.
    xTaskCreate(notify_task, "notify", 8192, NULL, 3, NULL);
}

void notify_ring(line_t line)
{
    if (s_queue) {
        uint8_t l = line;
        xQueueSend(s_queue, &l, 0);
    }
}
