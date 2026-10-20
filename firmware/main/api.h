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
    API_REMOTE,   // through the broker: no keys, settings or restarts, and replies everyone may see
} api_via_t;

// {"type":"state", ...}. `full`, for admins: door, call lines, connections and the recent
// activity. Otherwise the short state, all a phone from an ordinary link gets: what it
// needs to open the door.
cJSON *api_state(bool full);

// `text` sealed so that only the phone with key `key_id` can read it, for the broker:
// {"n": nonce, "m": tag, "x": base64}. See docs/protocol.md. Caller frees; NULL if the
// key is gone or memory ran out.
char *api_seal(uint32_t key_id, const char *text);

// Runs a signed command and returns the reply {"type":"reply","re":...,"ok":...}; the
// caller deletes it. *authed is set when the signature checked out (*who is then the key),
// whatever the command itself did.
cJSON *api_run(const char *envelope, size_t len, api_via_t via, key_info_t *who, bool *authed);

// Everything a phone needs to use `key`. Pairing links carry it as #pair=<base64url JSON>.
cJSON *api_pairing(const key_info_t *key, const char *secret_hex);
