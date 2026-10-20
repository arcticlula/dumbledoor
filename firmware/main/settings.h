#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "knock.h"

// Opening the door with a pattern of presses on the lobby panel's call button.
typedef struct {
    bool on;
    char seq[KNOCK_MAX + 1];   // '.' short press, '-' long press, e.g. "..-"
    uint16_t short_ms;         // presses shorter than this count as '.'
    uint16_t long_ms;          // presses at least this long count as '-'; in between, the code fails
    uint16_t gap_ms;           // this much silence after a press ends the code
} settings_code_t;

// Everything Dumbledoor remembers across power cycles, apart from the Wi-Fi network
// (kept by the Wi-Fi driver) and the phones' keys (keys.c).
typedef struct {
    char name[32];             // shown in the app and on push notifications
    settings_code_t code;

    // Remote access through an MQTT broker.
    bool mqtt_on;
    char mqtt_url[128];        // what the device connects to, e.g. mqtts://xxxx.emqxsl.com:8883
    char mqtt_user[64];
    char mqtt_pass[64];
    char mqtt_prefix[48];      // topics are <prefix>/<device id>/...
    char app_mqtt_url[128];    // what phones connect to, e.g. wss://xxxx.emqxsl.com:8084/mqtt
    char app_mqtt_user[64];
    char app_mqtt_pass[64];

    char app_url[128];         // where the online app is hosted; used in pairing links
    char ntfy_url[128];        // push notifications for rings, e.g. https://ntfy.sh/<topic>; empty = off
} dd_settings_t;

void settings_init(void);
void settings_get(dd_settings_t *out);
// Saves everything but the code, which only settings_set_code() changes. Admins change
// the code from anywhere and the rest at home, so neither can undo the other's change.
void settings_set(const dd_settings_t *s);
void settings_set_code(const settings_code_t *code);
void settings_get_code(settings_code_t *out);
void settings_get_name(char *out, size_t size);

// Goes up by one on every change, so tasks can notice changes cheaply.
uint32_t settings_generation(void);
