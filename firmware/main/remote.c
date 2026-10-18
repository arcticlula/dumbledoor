#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "api.h"
#include "remote.h"
#include "settings.h"
#include "util.h"
#include "web.h"

#define PUBLISH_DELAY_MS 100   // collects changes that arrive together into one message
#define TOPIC_LEN 96

static const char *TAG = "remote";

static TaskHandle_t s_task;
static esp_mqtt_client_handle_t s_client;   // only touched by remote_task
static volatile bool s_restart;
static volatile bool s_enabled;
static volatile bool s_connected;
// Fixed while a client exists; read by the MQTT task's event handler.
static char s_topic_state[TOPIC_LEN];
static char s_topic_status[TOPIC_LEN];
static char s_topic_cmd[TOPIC_LEN];
static char s_topic_reply[TOPIC_LEN];

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to the broker");
        s_connected = true;
        esp_mqtt_client_subscribe_single(e->client, s_topic_cmd, 0);
        esp_mqtt_client_enqueue(e->client, s_topic_status, "online", 0, 1, 1, true);
        remote_notify();   // publish the state
        web_notify();      // pages at home show the remote link is up
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "lost the broker");
        }
        s_connected = false;
        web_notify();
        break;
    case MQTT_EVENT_DATA:
        if (e->topic_len == (int)strlen(s_topic_cmd) && !memcmp(e->topic, s_topic_cmd, e->topic_len) &&
            e->data_len == e->total_data_len) {
            key_info_t who;
            bool authed;
            cJSON *reply = api_run(e->data, e->data_len, API_REMOTE, &who, &authed);
            char *json = cJSON_PrintUnformatted(reply);
            cJSON_Delete(reply);
            if (json) {
                esp_mqtt_client_enqueue(e->client, s_topic_reply, json, 0, 0, 0, true);
                free(json);
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "the broker refused the connection (code %d): check the user and password",
                     e->error_handle->connect_return_code);
        } else if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "can't reach the broker (tls 0x%x, socket errno %d)", e->error_handle->esp_tls_last_esp_err,
                     e->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

static void configure(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;

    dd_settings_t *s = malloc(sizeof(*s));
    if (!s) {
        return;
    }
    settings_get(s);
    s_enabled = s->mqtt_on && s->mqtt_url[0];
    if (s_enabled) {
        const char *id = util_device_id();
        snprintf(s_topic_state, TOPIC_LEN, "%s/%s/state", s->mqtt_prefix, id);
        snprintf(s_topic_status, TOPIC_LEN, "%s/%s/status", s->mqtt_prefix, id);
        snprintf(s_topic_cmd, TOPIC_LEN, "%s/%s/cmd", s->mqtt_prefix, id);
        snprintf(s_topic_reply, TOPIC_LEN, "%s/%s/reply", s->mqtt_prefix, id);
        // The client copies every string, so `s` can go once it's created.
        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = s->mqtt_url,
            .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
            .credentials.username = s->mqtt_user[0] ? s->mqtt_user : NULL,
            .credentials.client_id = id,
            .credentials.authentication.password = s->mqtt_pass[0] ? s->mqtt_pass : NULL,
            .session.last_will = { .topic = s_topic_status, .msg = "offline", .qos = 1, .retain = 1 },
            .session.keepalive = 60,
            .network.reconnect_timeout_ms = 15000,
            .buffer.size = 4096,   // the state with 20 events is ~2.5 KB
            .task.stack_size = 8192,
        };
        s_client = esp_mqtt_client_init(&cfg);
        if (s_client) {
            esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
            esp_mqtt_client_start(s_client);
            ESP_LOGI(TAG, "connecting to %s as %s", s->mqtt_url, id);
        } else {
            ESP_LOGE(TAG, "couldn't create the MQTT client; is the broker address right?");
        }
    }
    free(s);
    web_notify();
}

static void publish_state(void)
{
    cJSON *state = api_state();
    char *json = cJSON_PrintUnformatted(state);
    cJSON_Delete(state);
    if (json) {
        esp_mqtt_client_publish(s_client, s_topic_state, json, 0, 0, 1);
        free(json);
    }
}

static void remote_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_DELAY_MS));
        ulTaskNotifyTake(pdTRUE, 0);   // changes that came in meanwhile go out with this one
        if (s_restart) {
            s_restart = false;
            configure();
        }
        if (s_client && s_connected) {
            publish_state();
        }
    }
}

void remote_start(void)
{
    s_restart = true;
    xTaskCreate(remote_task, "remote", 6144, NULL, 4, &s_task);
    remote_notify();
}

void remote_restart(void)
{
    s_restart = true;
    remote_notify();
}

void remote_notify(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool remote_enabled(void)
{
    return s_enabled;
}

bool remote_connected(void)
{
    return s_connected;
}
