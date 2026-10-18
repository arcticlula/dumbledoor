#pragma once

// The wires to the intercom (dumbledoor/dumbledoor.kicad_sch): the call lines in,
// the monitor's call line out, and the door and audio-channel "buttons".

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LINE_LOBBY,   // terminal 7: the call from the panel at the building's entrance
    LINE_FRONT,   // terminal 20: the apartment's own doorbell
    LINE_COUNT,
} line_t;

void intercom_init(void);
bool intercom_has_line(line_t line);

// False until the GPIOs are confirmed in menuconfig (and don't clash). Until then the
// outputs are never driven, and the set_* functions do nothing.
bool intercom_outputs_ok(void);

// Whether a call was on the line since the last poll: edges of the call tone, or
// the line held at its active level. Poll every tick.
bool intercom_signal(line_t line);

// With the lobby input read through the ADC: the lowest and highest readings (0–4095)
// since the last call, for checking the thresholds. False when it's read digitally.
bool intercom_take_range(uint16_t *min, uint16_t *max);

// Whether lobby calls are passed through to the monitor. Off keeps it silent.
void intercom_set_bell(bool on);
bool intercom_has_bell(void);

void intercom_set_unlock(bool on);
void intercom_set_talk(bool on);   // does nothing if the audio-channel output isn't wired
bool intercom_has_talk(void);
