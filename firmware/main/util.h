#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// "dd-" plus the last three bytes of the Wi-Fi MAC, e.g. "dd-1a2b3c".
// Names the device in MQTT topics and in every signed command.
const char *util_device_id(void);

// Milliseconds since boot. Wraps after 49 days, so compare with subtraction.
uint32_t util_uptime_ms(void);

// True once `t` (an util_uptime_ms() value) has been reached. Wrap-safe.
static inline bool util_due(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

// Unix time in ms, or 0 while the clock hasn't been set over NTP.
int64_t util_wall_ms(void);

// Restarts the chip after `ms`, so a reply can still go out first.
void util_restart_soon(uint32_t ms);

// Lowercase hex. `out` needs 2 * len + 1 chars.
void util_hex(const uint8_t *in, size_t len, char *out);

// Parses exactly 2 * len hex digits.
bool util_unhex(const char *in, uint8_t *out, size_t len);
