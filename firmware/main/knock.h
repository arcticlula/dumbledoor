#pragma once

// Turns presses of the lobby call button into a code like "..-" (short, short, long).
// Plain C with no ESP-IDF dependencies, so firmware/test can run it on a computer.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KNOCK_MAX 12   // longest code

// Presses between short_ms and long_ms are '?': neither, so the code can't match. Better
// a clear mistake than a guess (the old Arduino sketches called these "invalid").
typedef struct {
    uint16_t short_ms;  // presses shorter than this are '.'
    uint16_t long_ms;   // presses at least this long are '-'
    uint16_t gap_ms;    // this much silence after a press ends the code
} knock_timing_t;

typedef struct {
    char seq[KNOCK_MAX + 1];
    uint8_t len;        // can exceed KNOCK_MAX; only the first KNOCK_MAX presses are kept
    bool pressing;
    uint32_t last_ms;   // when the last press ended
} knock_t;

void knock_reset(knock_t *k);
void knock_press_start(knock_t *k);
void knock_press_end(knock_t *k, const knock_timing_t *t, uint32_t duration_ms, uint32_t now_ms);

// Once the gap has passed after the last press: copies the code into `out`, starts over,
// and returns the number of presses. Otherwise returns 0.
int knock_poll(knock_t *k, const knock_timing_t *t, uint32_t now_ms, char *out, size_t out_size);
