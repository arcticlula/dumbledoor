#pragma once

#include <stdint.h>

// The activity list the app shows. Kept in RAM only, so it starts empty after a restart.

typedef enum {
    EV_BOOT,        // who: reset reason
    EV_RING,        // line, value: how long the button was held (ms)
    EV_OPEN,        // who: key name, "code" or "auto-open"; via: "local", "remote", "panel", "auto"
    EV_AUTO,        // who turned auto-open on or off; value: minutes (0 = off)
    EV_MUTE,        // who muted or unmuted the monitor; value: minutes (0 = off, UINT32_MAX = until unmuted)
    EV_CODE_FAIL,   // a wrong code at the lobby panel; value: wrong codes in a row; who: the presses ('.', '-', '?')
    EV_DENIED,      // a command that failed its signature check; who: reason; via
    EV_KEY_ADD,     // who: the new key's name
    EV_KEY_REMOVE,  // who: the removed key's name
    EV_TYPE_COUNT,
} event_type_t;

typedef struct {
    uint32_t n;        // sequence number, counts up from 1 at boot
    int64_t time_ms;   // Unix time, 0 if the clock wasn't set yet
    uint32_t up_ms;    // uptime
    uint8_t type;      // event_type_t
    uint8_t line;      // line_t, for EV_RING
    uint32_t value;
    char who[24];
    char via[8];
} event_t;

#define EVENTS_KEEP 20

// Stamps `e` with its number and times, stores it, and tells the listener. Safe from any task.
void events_post(event_t *e);

// Copies up to `max` of the most recent events into `out`, oldest first. Returns the count.
int events_recent(event_t *out, int max);

void events_set_listener(void (*fn)(void));
const char *events_type_name(uint8_t type);
