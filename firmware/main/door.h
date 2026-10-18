#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "intercom.h"

typedef struct {
    bool outputs_ok;             // GPIOs confirmed: the door and the monitor's line can be driven
    bool has_line[LINE_COUNT];   // wired call inputs
    bool has_bell;               // lobby calls go through the device to the monitor
    bool ringing[LINE_COUNT];    // someone is holding the call button right now
    bool opening;                // the door release is active
    uint32_t auto_left_s;        // every lobby ring opens the door for this much longer
    int32_t mute_left_s;         // the monitor stays silent for this much longer; -1 = until unmuted
    bool code_on;                // codes at the lobby panel are accepted
    uint32_t code_locked_s;      // code entry paused after wrong codes, for this much longer
} door_state_t;

// Starts the task that watches the call lines and drives the outputs.
void door_start(void);

// Releases the lobby door. `who` and `via` go into the activity list.
// ESP_ERR_INVALID_STATE while the door is opening or cooling down, ESP_ERR_NOT_SUPPORTED
// until the GPIOs are confirmed. Safe from any task.
esp_err_t door_open(const char *who, const char *via);

// Opens the door on every lobby ring for the next `minutes` (0 turns it off).
void door_set_auto(uint32_t minutes, const char *who);

// Keeps the monitor from ringing for the next `minutes` (0 = ring again, -1 = until
// told otherwise or restarted). Phones are still told about every ring.
void door_set_mute(int32_t minutes, const char *who);

void door_get_state(door_state_t *out);

// Called (from the door task) when door_state_t changes.
void door_set_listener(void (*fn)(void));

const char *door_line_name(int line);
