#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "board.h"
#include "door.h"
#include "events.h"
#include "keys.h"
#include "knock.h"
#include "net.h"
#include "notify.h"
#include "settings.h"
#include "util.h"

#define TICK_MS 10
#define PRESS_MIN_MS 20                        // this much signal starts a press
#define PRESS_MAX_MS 20000                     // a longer "press" means the line is stuck
#define AUTO_OPEN_DELAY_MS 500                 // auto-open waits this long after the press
#define CODE_ATTEMPT_LEN 3                     // fewer presses are an ordinary ring, not a wrong code
#define CODE_MAX_FAILS 5
#define CODE_FAIL_WINDOW_MS (15 * 60 * 1000)
#define CODE_LOCK_MS (15 * 60 * 1000)
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_SETUP_MS 5000
#define CLAIM_WINDOW_MS (10 * 60 * 1000)

static const char *TAG = "door";
static const char *const LINE_NAMES[LINE_COUNT] = { "lobby", "front" };

typedef struct {
    bool active;
    bool stuck;          // signal for PRESS_MAX_MS: ignored until the line goes quiet
    uint8_t on_ticks;    // ticks in a row with signal, while idle
    uint32_t start_ms;
    uint32_t last_ms;    // last tick with signal
} press_t;

typedef enum {
    PHASE_IDLE,
    PHASE_TALK,    // audio channel open, door release next
    PHASE_PULSE,   // door release active
} phase_t;

// What the debug LEDs are flashing, on top of counting presses.
typedef enum {
    FLASH_NONE,
    FLASH_RIGHT,     // the greens, three times: the code was right
    FLASH_WRONG,     // the red one, three times: a wrong code
    FLASH_UNCLEAR,   // the red one, once: a press between short and long
} flash_t;

#define FLASH_STEP_MS 150   // as in the old sketches

// Shared with other tasks, guarded by s_mux.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static door_state_t s_state;
static bool s_open_pending;
static char s_open_who[24];
static char s_open_via[8];
static bool s_opened_before;
static uint32_t s_opened_ms;   // when the last opening was accepted
static bool s_auto_on;
static uint32_t s_auto_until;
static bool s_mute_on;
static bool s_mute_forever;
static uint32_t s_mute_until;
static bool s_code_locked;
static uint32_t s_code_locked_until;

// Only used by the door task.
static void (*s_listener)(void);
static press_t s_press[LINE_COUNT];
static knock_t s_knock;
static knock_timing_t s_timing;
static char s_code[KNOCK_MAX + 1];
static bool s_code_on;
static uint32_t s_settings_gen;
static uint32_t s_fails;
static uint32_t s_first_fail_ms;
static bool s_auto_pending;
static uint32_t s_auto_at;
static phase_t s_phase;
static uint32_t s_phase_end;
static bool s_button_raw;
static bool s_button_down;
static uint32_t s_button_changed_ms;
static uint32_t s_button_down_ms;
static bool s_button_fired;
static bool s_flash;
static uint32_t s_flash_until;
static bool s_led;
static uint8_t s_presses;   // lobby presses since the last code ended, for the debug LEDs
static uint32_t s_held_ms[KNOCK_MAX];   // lobby presses held back while the code is on
static uint8_t s_held;
static flash_t s_debug_flash;
static uint32_t s_debug_since;
static bool s_idle_logged;

// While the lobby code is on, phones hear nothing of lobby presses until they stop: the
// timing of each update would give the code away. Then poll_code() reports one outcome:
// the door opening (the right code), a wrong code with its pattern, or these presses as
// ordinary rings when there weren't enough for a code. Auto-open waits for that too.
static void post_held(void)
{
    for (int i = 0; i < s_held; i++) {
        event_t e = { .type = EV_RING, .line = LINE_LOBBY, .value = s_held_ms[i] };
        events_post(&e);
    }
    s_held = 0;
}

static bool refresh_settings(void)
{
    uint32_t gen = settings_generation();
    if (gen == s_settings_gen) {
        return false;
    }
    s_settings_gen = gen;
    post_held();   // the knock starts over below
    settings_code_t code;
    settings_get_code(&code);
    s_code_on = code.on && code.seq[0];
    strlcpy(s_code, code.seq, sizeof(s_code));
    s_timing.short_ms = code.short_ms;
    s_timing.long_ms = code.long_ms;
    s_timing.gap_ms = code.gap_ms;
    knock_reset(&s_knock);
    portENTER_CRITICAL(&s_mux);
    s_state.code_on = s_code_on;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

static void debug_flash(flash_t kind, uint32_t now)
{
    s_debug_flash = kind;
    s_debug_since = now;
}

// Both return whether phones see the change.
static bool press_started(line_t line)
{
    ESP_LOGI(TAG, "%s: ringing", LINE_NAMES[line]);
    portENTER_CRITICAL(&s_mux);
    s_state.ringing[line] = true;
    portEXIT_CRITICAL(&s_mux);
    if (line == LINE_LOBBY) {
        uint16_t lo, hi;
        intercom_take_range(&lo, &hi);   // start this press's reading range afresh
        knock_press_start(&s_knock);
        if (s_presses < UINT8_MAX) {
            s_presses++;
        }
    }
    notify_ring(line);
    return line != LINE_LOBBY || !s_code_on;   // door_get_state() hides it
}

static bool press_ended(line_t line, uint32_t duration, uint32_t now)
{
    uint16_t lo, hi;
    if (line == LINE_LOBBY && intercom_take_range(&lo, &hi)) {
        ESP_LOGI(TAG, "lobby: held for %" PRIu32 " ms (input between %u and %u of 4095)", duration, lo, hi);
    } else {
        ESP_LOGI(TAG, "%s: held for %" PRIu32 " ms", LINE_NAMES[line], duration);
    }
    if (line == LINE_LOBBY && duration >= s_timing.short_ms && duration < s_timing.long_ms) {
        debug_flash(FLASH_UNCLEAR, now);
    }
    portENTER_CRITICAL(&s_mux);
    s_state.ringing[line] = false;
    bool auto_open = s_auto_on && !util_due(now, s_auto_until);
    portEXIT_CRITICAL(&s_mux);

    if (line == LINE_LOBBY) {
        knock_press_end(&s_knock, &s_timing, duration, now);
        if (s_code_on) {
            if (s_held < KNOCK_MAX) {
                s_held_ms[s_held++] = duration;
            }
            return false;   // poll_code() reports it, and auto-opens, once the presses stop
        }
        if (auto_open) {
            s_auto_pending = true;
            s_auto_at = now + AUTO_OPEN_DELAY_MS;
        }
    }
    event_t e = { .type = EV_RING, .line = line, .value = duration };
    events_post(&e);
    return true;
}

static bool poll_line(line_t line, uint32_t now)
{
    if (!s_state.has_line[line]) {
        return false;
    }
    press_t *p = &s_press[line];
    bool signal = intercom_signal(line);
    if (!p->active) {
        if (!signal) {
            p->on_ticks = 0;
            p->stuck = false;
            return false;
        }
        if (p->stuck || ++p->on_ticks < PRESS_MIN_MS / TICK_MS) {
            return false;
        }
        p->active = true;
        p->start_ms = now - (p->on_ticks - 1) * TICK_MS;
        p->last_ms = now;
        return press_started(line);
    }
    if (signal) {
        p->last_ms = now;
        if (now - p->start_ms < PRESS_MAX_MS) {
            return false;
        }
        ESP_LOGW(TAG, "%s line busy for %d s, ignoring it until it goes quiet", LINE_NAMES[line],
                 PRESS_MAX_MS / 1000);
        p->stuck = true;
    } else if (now - p->last_ms < CONFIG_DUMBLEDOOR_RING_RELEASE_MS) {
        return false;   // a gap in the tone, not the end of the press
    }
    p->active = false;
    p->on_ticks = 0;
    return press_ended(line, p->last_ms - p->start_ms + TICK_MS, now);
}

// Returns whether code entry got paused.
static bool code_failed(const char *seq, uint32_t now)
{
    s_held = 0;
    debug_flash(FLASH_WRONG, now);
    if (s_fails == 0 || util_due(now, s_first_fail_ms + CODE_FAIL_WINDOW_MS)) {
        s_fails = 0;
        s_first_fail_ms = now;
    }
    s_fails++;
    ESP_LOGW(TAG, "wrong code (%" PRIu32 " in a row)", s_fails);
    event_t e = { .type = EV_CODE_FAIL, .value = s_fails };
    strlcpy(e.who, seq, sizeof(e.who));   // the presses as read: '.', '-' and '?'
    events_post(&e);
    if (s_fails < CODE_MAX_FAILS) {
        return false;
    }
    ESP_LOGW(TAG, "pausing code entry for %d min", CODE_LOCK_MS / 60000);
    s_fails = 0;
    portENTER_CRITICAL(&s_mux);
    s_code_locked = true;
    s_code_locked_until = now + CODE_LOCK_MS;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

static bool poll_code(uint32_t now)
{
    bool changed = false;
    portENTER_CRITICAL(&s_mux);
    if (s_code_locked && util_due(now, s_code_locked_until)) {
        s_code_locked = false;
        changed = true;
    }
    bool locked = s_code_locked;
    bool auto_open = s_auto_on && !util_due(now, s_auto_until);
    portEXIT_CRITICAL(&s_mux);

    char seq[KNOCK_MAX + 1];
    int len = knock_poll(&s_knock, &s_timing, now, seq, sizeof(seq));
    if (len == 0) {
        return changed;
    }
    s_presses = 0;   // the presses are over, one way or another
    if (!s_code_on) {
        post_held();
        return changed;
    }
    size_t code_len = strlen(s_code);
    if (locked) {
        ESP_LOGW(TAG, "code entry is paused after wrong codes");
        debug_flash(FLASH_WRONG, now);
        post_held();
    } else if ((size_t)len == code_len && strcmp(seq, s_code) == 0) {
        ESP_LOGI(TAG, "code accepted");
        s_fails = 0;
        debug_flash(FLASH_RIGHT, now);
        if (door_open("code", "panel") == ESP_OK) {
            s_held = 0;   // the opening says it all; the presses are the code itself
            return changed;
        }
        post_held();
    } else if ((size_t)len < (code_len < CODE_ATTEMPT_LEN ? code_len : CODE_ATTEMPT_LEN)) {
        post_held();   // an ordinary ring
    } else {
        changed |= code_failed(seq, now);
    }
    if (auto_open) {
        door_open("auto-open", "auto");
    }
    return changed;
}

static bool poll_auto(uint32_t now)
{
    bool changed = false;
    portENTER_CRITICAL(&s_mux);
    if (s_auto_on && util_due(now, s_auto_until)) {
        s_auto_on = false;
        changed = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (s_auto_pending && util_due(now, s_auto_at)) {
        s_auto_pending = false;
        door_open("auto-open", "auto");
    }
    return changed;
}

static bool poll_mute(uint32_t now)
{
    portENTER_CRITICAL(&s_mux);
    bool expired = s_mute_on && !s_mute_forever && util_due(now, s_mute_until);
    if (expired) {
        s_mute_on = false;
    }
    portEXIT_CRITICAL(&s_mux);
    if (expired) {
        ESP_LOGI(TAG, "mute is over, the monitor rings again");
        intercom_set_bell(true);
    }
    return expired;
}

static bool run_open(uint32_t now)
{
    switch (s_phase) {
    case PHASE_IDLE: {
        char who[sizeof(s_open_who)];
        char via[sizeof(s_open_via)];
        portENTER_CRITICAL(&s_mux);
        bool go = s_open_pending;
        if (go) {
            s_open_pending = false;
            s_state.opening = true;
            memcpy(who, s_open_who, sizeof(who));
            memcpy(via, s_open_via, sizeof(via));
        }
        portEXIT_CRITICAL(&s_mux);
        if (!go) {
            return false;
        }
        ESP_LOGI(TAG, "opening for %s (%s)", who, via);
        event_t e = { .type = EV_OPEN };
        strlcpy(e.who, who, sizeof(e.who));
        strlcpy(e.via, via, sizeof(e.via));
        events_post(&e);
        if (intercom_has_talk() && CONFIG_DUMBLEDOOR_TALK_LEAD_MS > 0) {
            intercom_set_talk(true);
            s_phase = PHASE_TALK;
            s_phase_end = now + CONFIG_DUMBLEDOOR_TALK_LEAD_MS;
        } else {
            intercom_set_unlock(true);
            s_phase = PHASE_PULSE;
            s_phase_end = now + CONFIG_DUMBLEDOOR_UNLOCK_PULSE_MS;
        }
        return true;
    }
    case PHASE_TALK:
        if (util_due(now, s_phase_end)) {
            intercom_set_unlock(true);
            s_phase = PHASE_PULSE;
            s_phase_end = now + CONFIG_DUMBLEDOOR_UNLOCK_PULSE_MS;
        }
        return false;
    case PHASE_PULSE:
        if (!util_due(now, s_phase_end)) {
            return false;
        }
        intercom_set_unlock(false);
        intercom_set_talk(false);
        s_phase = PHASE_IDLE;
        portENTER_CRITICAL(&s_mux);
        s_state.opening = false;
        portEXIT_CRITICAL(&s_mux);
        return true;
    }
    return false;
}

static void poll_button(uint32_t now)
{
    bool raw = board_button_down();
    if (raw != s_button_raw) {
        s_button_raw = raw;
        s_button_changed_ms = now;
    } else if (raw != s_button_down && now - s_button_changed_ms >= BUTTON_DEBOUNCE_MS) {
        s_button_down = raw;
        if (raw) {
            s_button_down_ms = now;
            s_button_fired = false;
        }
    }
    if (s_button_down && !s_button_fired && now - s_button_down_ms >= BUTTON_SETUP_MS) {
        s_button_fired = true;
        ESP_LOGI(TAG, "button held: opening Wi-Fi setup and letting a new admin phone pair");
        net_start_setup();
        keys_allow_claim(CLAIM_WINDOW_MS);
        s_flash = true;
        s_flash_until = now + 1500;
    }
}

static void update_led(uint32_t now)
{
    if (s_flash && util_due(now, s_flash_until)) {
        s_flash = false;
    }
    bool ringing = s_state.ringing[LINE_LOBBY] || s_state.ringing[LINE_FRONT];
    bool on;
    if (s_phase != PHASE_IDLE) {
        on = true;                                    // door opening
    } else if (s_flash) {
        on = (now / 100) % 2;                         // button hold accepted
    } else if (s_button_down && now - s_button_down_ms >= 1000) {
        on = true;                                    // keep holding…
    } else if (ringing) {
        on = (now / 150) % 2;
    } else if (net_setup_active()) {
        on = (now / 500) % 2;
    } else if (!net_connected()) {
        on = now % 2000 < 60;                         // a blip every 2 s while Wi-Fi is down
    } else {
        on = false;
    }
    if (on != s_led) {
        s_led = on;
        board_led(on);
    }
}

// Like the old sketches: a green LED per press of the code being entered, all of them
// flashing when it's right, the red one flashing when it's wrong (and on while code entry
// is paused).
static void update_debug_leds(uint32_t now)
{
    uint32_t t = now - s_debug_since;
    bool step_on = (t / FLASH_STEP_MS) % 2 == 0;
    uint8_t count = s_presses < 3 ? s_presses : 3;
    uint8_t greens = (1u << count) - 1;
    bool red = s_code_locked;
    switch (s_debug_flash) {
    case FLASH_RIGHT:
        greens = step_on ? 0x07 : 0;
        break;
    case FLASH_WRONG:
        greens = 0;
        red = step_on;
        break;
    case FLASH_UNCLEAR:
        red = t < 2 * FLASH_STEP_MS;
        break;
    default:
        break;
    }
    if (s_debug_flash != FLASH_NONE && t >= 6 * FLASH_STEP_MS) {
        s_debug_flash = FLASH_NONE;
    }
    board_debug_leds(greens, red);
}

static void log_idle_level(uint32_t now)
{
    uint16_t lo, hi;
    if (s_idle_logged || now < 3000) {
        return;
    }
    s_idle_logged = true;
    if (intercom_take_range(&lo, &hi)) {
        ESP_LOGI(TAG, "lobby input at rest: between %u and %u of 4095", lo, hi);
    }
}

static void door_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
        uint32_t now = util_uptime_ms();
        bool changed = refresh_settings();
        for (int line = 0; line < LINE_COUNT; line++) {
            changed |= poll_line(line, now);
        }
        changed |= poll_code(now);
        changed |= poll_auto(now);
        changed |= poll_mute(now);
        changed |= run_open(now);
        poll_button(now);
        update_led(now);
        update_debug_leds(now);
        log_idle_level(now);
        if (changed && s_listener) {
            s_listener();
        }
    }
}

void door_start(void)
{
    intercom_init();
    board_init(intercom_outputs_ok());
    for (int line = 0; line < LINE_COUNT; line++) {
        s_state.has_line[line] = intercom_has_line(line);
    }
    s_state.has_bell = intercom_has_bell();
    s_state.outputs_ok = intercom_outputs_ok();
    knock_reset(&s_knock);
    xTaskCreate(door_task, "door", 4096, NULL, 6, NULL);
}

esp_err_t door_open(const char *who, const char *via)
{
    if (!intercom_outputs_ok()) {
        ESP_LOGW(TAG, "not opening for %s: the GPIOs aren't confirmed in menuconfig", who);
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t now = util_uptime_ms();
    portENTER_CRITICAL(&s_mux);
    bool busy = s_open_pending || s_state.opening ||
                (s_opened_before && now - s_opened_ms < CONFIG_DUMBLEDOOR_COOLDOWN_MS);
    if (!busy) {
        s_open_pending = true;
        s_opened_before = true;
        s_opened_ms = now;
        strlcpy(s_open_who, who, sizeof(s_open_who));
        strlcpy(s_open_via, via, sizeof(s_open_via));
    }
    portEXIT_CRITICAL(&s_mux);
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}

void door_set_auto(uint32_t minutes, const char *who)
{
    uint32_t now = util_uptime_ms();
    portENTER_CRITICAL(&s_mux);
    s_auto_on = minutes > 0;
    s_auto_until = now + minutes * 60 * 1000;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "auto-open %s by %s", minutes ? "on" : "off", who);
    event_t e = { .type = EV_AUTO, .value = minutes };
    strlcpy(e.who, who, sizeof(e.who));
    events_post(&e);
}

void door_set_mute(int32_t minutes, const char *who)
{
    uint32_t now = util_uptime_ms();
    portENTER_CRITICAL(&s_mux);
    s_mute_on = minutes != 0;
    s_mute_forever = minutes < 0;
    s_mute_until = now + (minutes > 0 ? (uint32_t)minutes * 60 * 1000 : 0);
    portEXIT_CRITICAL(&s_mux);
    intercom_set_bell(minutes == 0);
    ESP_LOGI(TAG, "monitor %s by %s", minutes ? "muted" : "unmuted", who);
    event_t e = { .type = EV_MUTE, .value = minutes < 0 ? UINT32_MAX : (uint32_t)minutes };
    strlcpy(e.who, who, sizeof(e.who));
    events_post(&e);
}

void door_get_state(door_state_t *out)
{
    uint32_t now = util_uptime_ms();
    portENTER_CRITICAL(&s_mux);
    *out = s_state;
    if (out->code_on) {
        out->ringing[LINE_LOBBY] = false;   // reported once the presses stop (poll_code)
    }
    out->auto_left_s = s_auto_on && !util_due(now, s_auto_until) ? (s_auto_until - now + 999) / 1000 : 0;
    out->mute_left_s = !s_mute_on               ? 0
                       : s_mute_forever         ? -1
                       : util_due(now, s_mute_until) ? 0
                                                     : (int32_t)((s_mute_until - now + 999) / 1000);
    out->code_locked_s =
        s_code_locked && !util_due(now, s_code_locked_until) ? (s_code_locked_until - now + 999) / 1000 : 0;
    portEXIT_CRITICAL(&s_mux);
}

void door_set_listener(void (*fn)(void))
{
    s_listener = fn;
}

const char *door_line_name(int line)
{
    return line >= 0 && line < LINE_COUNT ? LINE_NAMES[line] : "unknown";
}
