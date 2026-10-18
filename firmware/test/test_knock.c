// Host test for main/knock.c, the lobby-code decoder. Run with: sh firmware/test/run.sh
#include <stdio.h>
#include <string.h>
#include "knock.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

static const knock_timing_t T = { .short_ms = 400, .long_ms = 700, .gap_ms = 1500 };

// One press of `ms`, starting at *now; leaves *now just after it.
static void press(knock_t *k, uint32_t *now, uint32_t ms)
{
    knock_press_start(k);
    *now += ms;
    knock_press_end(k, &T, ms, *now);
    *now += 300;   // a short pause before the next press
}

int main(void)
{
    knock_t k;
    char out[KNOCK_MAX + 1];
    uint32_t now = 1000;

    // click, click, hold
    knock_reset(&k);
    press(&k, &now, 200);
    press(&k, &now, 250);
    press(&k, &now, 900);
    CHECK(knock_poll(&k, &T, now, out, sizeof(out)) == 0);   // the gap hasn't passed
    now += 1500;
    CHECK(knock_poll(&k, &T, now, out, sizeof(out)) == 3);
    CHECK(strcmp(out, "..-") == 0);
    CHECK(knock_poll(&k, &T, now + 5000, out, sizeof(out)) == 0);   // reported once

    // exactly long_ms is long, just under short_ms is short, and in between is neither
    knock_reset(&k);
    press(&k, &now, 700);
    press(&k, &now, 399);
    press(&k, &now, 400);
    press(&k, &now, 699);
    now += 2000;
    CHECK(knock_poll(&k, &T, now, out, sizeof(out)) == 4);
    CHECK(strcmp(out, "-.??") == 0);

    // an unclear press spoils the code instead of being guessed
    knock_reset(&k);
    press(&k, &now, 200);
    press(&k, &now, 550);
    press(&k, &now, 900);
    now += 1500;
    CHECK(knock_poll(&k, &T, now, out, sizeof(out)) == 3);
    CHECK(strcmp(out, ".?-") == 0 && strcmp(out, "..-") != 0);

    // a press that is still going doesn't end the code, however long the gap
    knock_reset(&k);
    press(&k, &now, 200);
    knock_press_start(&k);
    now += 5000;
    CHECK(knock_poll(&k, &T, now, out, sizeof(out)) == 0);
    knock_press_end(&k, &T, 5000, now);
    now += 1500;
    CHECK(knock_poll(&k, &T, now, out, sizeof(out)) == 2);
    CHECK(strcmp(out, ".-") == 0);

    // more presses than fit: counted, but only the first KNOCK_MAX are kept
    knock_reset(&k);
    for (int i = 0; i < KNOCK_MAX + 3; i++) {
        press(&k, &now, 100);
    }
    now += 1500;
    CHECK(knock_poll(&k, &T, now, out, sizeof(out)) == KNOCK_MAX + 3);
    CHECK(strlen(out) == KNOCK_MAX);

    // the uptime counter wrapping in the middle of a code
    knock_reset(&k);
    now = UINT32_MAX - 500;
    press(&k, &now, 200);   // ends just before the wrap
    press(&k, &now, 900);   // ends after it
    now += 1500;
    CHECK(knock_poll(&k, &T, now, out, sizeof(out)) == 2);
    CHECK(strcmp(out, ".-") == 0);

    // nothing pressed, nothing reported
    knock_reset(&k);
    CHECK(knock_poll(&k, &T, now + 10000, out, sizeof(out)) == 0);

    printf(failures ? "knock: %d failures\n" : "knock: all good\n", failures);
    return failures != 0;
}
