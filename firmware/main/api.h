#pragma once

// What the app and the device say to each other, whichever way they're connected
// (WebSocket on the home network, or MQTT through a broker). See docs/protocol.md.

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "keys.h"

#define API_VERSION 1

typedef enum {
    API_LOCAL,    // home network: every command
    API_REMOTE,   // through the broker: opening the door and auto-open only
} api_via_t;

// {"type":"state", ...}: door, call lines, connections and the recent activity.
cJSON *api_state(void);

// Runs a signed command and returns the reply {"type":"reply","re":...,"ok":...}; the
// caller deletes it. *authed is set when the signature checked out (*who is then the key),
// whatever the command itself did.
cJSON *api_run(const char *envelope, size_t len, api_via_t via, key_info_t *who, bool *authed);

// Everything a phone needs to use `key`. Pairing links carry it as #pair=<base64url JSON>.
cJSON *api_pairing(const key_info_t *key, const char *secret_hex);
