#include "freertos/FreeRTOS.h"
#include "events.h"
#include "util.h"

static const char *const NAMES[EV_TYPE_COUNT] = {
    [EV_BOOT] = "boot",
    [EV_RING] = "ring",
    [EV_OPEN] = "open",
    [EV_AUTO] = "auto",
    [EV_MUTE] = "mute",
    [EV_CODE_FAIL] = "code_fail",
    [EV_DENIED] = "denied",
    [EV_KEY_ADD] = "key_add",
    [EV_KEY_REMOVE] = "key_remove",
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static event_t s_ring[EVENTS_KEEP];   // guarded by s_mux; event n lives at n % EVENTS_KEEP
static uint32_t s_next = 1;
static void (*s_listener)(void);

void events_post(event_t *e)
{
    e->time_ms = util_wall_ms();
    e->up_ms = util_uptime_ms();
    portENTER_CRITICAL(&s_mux);
    e->n = s_next++;
    s_ring[e->n % EVENTS_KEEP] = *e;
    portEXIT_CRITICAL(&s_mux);
    if (s_listener) {
        s_listener();
    }
}

int events_recent(event_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t last = s_next - 1;
    int count = last < EVENTS_KEEP ? (int)last : EVENTS_KEEP;
    if (count > max) {
        count = max;
    }
    for (int i = 0; i < count; i++) {
        out[i] = s_ring[(last - count + 1 + i) % EVENTS_KEEP];
    }
    portEXIT_CRITICAL(&s_mux);
    return count;
}

void events_set_listener(void (*fn)(void))
{
    s_listener = fn;
}

const char *events_type_name(uint8_t type)
{
    return type < EV_TYPE_COUNT ? NAMES[type] : "unknown";
}
