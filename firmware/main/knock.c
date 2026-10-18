#include <string.h>
#include "knock.h"

void knock_reset(knock_t *k)
{
    memset(k, 0, sizeof(*k));
}

void knock_press_start(knock_t *k)
{
    k->pressing = true;   // the gap only counts while nobody is pressing
}

void knock_press_end(knock_t *k, const knock_timing_t *t, uint32_t duration_ms, uint32_t now_ms)
{
    k->pressing = false;
    k->last_ms = now_ms;
    if (k->len < KNOCK_MAX) {
        k->seq[k->len] = duration_ms < t->short_ms ? '.' : duration_ms >= t->long_ms ? '-' : '?';
        k->seq[k->len + 1] = '\0';
    }
    if (k->len < UINT8_MAX) {
        k->len++;
    }
}

int knock_poll(knock_t *k, const knock_timing_t *t, uint32_t now_ms, char *out, size_t out_size)
{
    if (k->len == 0 || k->pressing || now_ms - k->last_ms < t->gap_ms) {
        return 0;
    }
    int len = k->len;
    strlcpy(out, k->seq, out_size);
    knock_reset(k);
    return len;
}
