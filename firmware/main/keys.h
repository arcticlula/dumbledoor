#pragma once

// The phones allowed to open the door. Each has a random 32-byte secret it signs
// commands with (HMAC-SHA256), so the secret never travels and a captured command
// can't be replayed: every command carries a timestamp newer than the last one.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#define KEYS_MAX 12
#define KEY_NAME_LEN 24
#define KEY_SECRET_LEN 32

typedef struct {
    uint32_t id;               // shown as 8 hex digits
    char name[KEY_NAME_LEN];
    bool admin;                // may add and remove keys and change settings
    int64_t last_t;            // newest command timestamp accepted from it (phone's clock, ms)
} key_info_t;

typedef enum {
    AUTH_OK,
    AUTH_BAD_REQUEST,     // not a signed command
    AUTH_UNKNOWN_KEY,     // removed, or never added
    AUTH_BAD_SIGNATURE,
    AUTH_WRONG_DEVICE,    // signed for another Dumbledoor
    AUTH_REPLAY,          // timestamp not newer than the last one from this key
    AUTH_CLOCK,           // timestamp more than 2 minutes off the device's clock
} auth_result_t;

void keys_init(void);
int keys_count(void);
bool keys_exists(uint32_t id);
int keys_list(key_info_t *out, int max);

// Checks a signed command {"k": key id, "p": payload JSON string, "s": HMAC of p}.
// On success *payload holds the parsed "p" (free it with cJSON_Delete) and *who the key.
// `re` (17 chars) gets the start of the signature whenever the command could be read,
// so the reply can be matched to it.
auth_result_t keys_verify(const char *envelope, size_t len, cJSON **payload, key_info_t *who, char *re);
const char *keys_error(auth_result_t r);

// `secret_hex` (65 chars) gets the new key's secret.
esp_err_t keys_add(const char *name, bool admin, key_info_t *out, char *secret_hex);
// ESP_ERR_NOT_FOUND, or ESP_ERR_INVALID_STATE for the last admin key.
esp_err_t keys_remove(uint32_t id);

// Claiming an admin key without already having one needs someone at the device:
// the window opens at boot while there are no keys, and when the setup button is held.
void keys_allow_claim(uint32_t window_ms);
bool keys_claim_allowed(void);
// ESP_ERR_INVALID_STATE when claiming isn't allowed right now. Closes the window.
esp_err_t keys_claim(const char *name, key_info_t *out, char *secret_hex);
