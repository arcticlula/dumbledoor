#include <stdint.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "intercom.h"
#if CONFIG_DUMBLEDOOR_LOBBY_ADC
#include "esp_adc/adc_continuous.h"
#endif

#define MIN_EDGES 2   // fewer edges between two polls is noise, not the ~570 Hz tone

#if CONFIG_DUMBLEDOOR_LOBBY_ACTIVE_LOW
#define LOBBY_ACTIVE 0
#else
#define LOBBY_ACTIVE 1
#endif

#if CONFIG_DUMBLEDOOR_FRONT_ACTIVE_HIGH
#define FRONT_ACTIVE 1
#else
#define FRONT_ACTIVE 0
#endif

#if CONFIG_DUMBLEDOOR_OUTPUTS_ACTIVE_HIGH
#define OUTPUT_ON 1
#else
#define OUTPUT_ON 0
#endif

#if CONFIG_DUMBLEDOOR_PINS_SET
#define PINS_CONFIRMED true
#else
#define PINS_CONFIRMED false
#endif

// bell_pin drives Q3, so its level is the opposite of the monitor's call line:
// low lets R8 pull the line up to ~12 V, high holds it at 0 V (its idle state).
#define BELL_LINE_HIGH 0
#define BELL_LINE_IDLE 1

static const char *TAG = "intercom";

static const int s_gpio[LINE_COUNT] = { CONFIG_DUMBLEDOOR_LOBBY_GPIO, CONFIG_DUMBLEDOOR_FRONT_GPIO };
static const int s_active[LINE_COUNT] = { LOBBY_ACTIVE, FRONT_ACTIVE };
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_edges[LINE_COUNT];   // guarded by s_mux
static volatile bool s_bell;
static bool s_has_line[LINE_COUNT];
static bool s_outputs_ok;
static bool s_lobby_adc;   // the lobby input is read through the ADC, not as a digital pin

// Runs on every edge of a call line read digitally. In IRAM, so the tone keeps reaching
// the monitor while flash is being written (it touches no flash-resident data or code).
static void IRAM_ATTR on_edge(void *arg)
{
    int line = (int)(intptr_t)arg;
    portENTER_CRITICAL_ISR(&s_mux);
    s_edges[line]++;
#if CONFIG_DUMBLEDOOR_BELL_GPIO >= 0
    if (line == LINE_LOBBY && s_bell) {
        // Copy the tone to the monitor's side: line high on the panel's side, line high on ours.
        bool high = gpio_get_level(CONFIG_DUMBLEDOOR_LOBBY_GPIO) == LOBBY_ACTIVE;
        gpio_set_level(CONFIG_DUMBLEDOOR_BELL_GPIO, high ? BELL_LINE_HIGH : BELL_LINE_IDLE);
    }
#endif
    portEXIT_CRITICAL_ISR(&s_mux);
}

#if CONFIG_DUMBLEDOOR_LOBBY_ADC
// The lobby input through the ADC, like the 18 Sep 2025 sketch: 20,000 samples a second
// (~35 per cycle of the call tone) through a software Schmitt trigger, counting edges.
#define ADC_RATE_HZ 20000
#define ADC_FRAME_BYTES 256   // 128 samples: one frame every 6.4 ms

static adc_continuous_handle_t s_adc;
static adc_channel_t s_adc_channel;
static volatile bool s_adc_low;   // below the low threshold at the end of the last frame
static uint16_t s_adc_min = UINT16_MAX;   // range seen since intercom_take_range(), guarded by s_mux
static uint16_t s_adc_max;

static void adc_task(void *arg)
{
    static uint8_t buf[ADC_FRAME_BYTES];
    bool low = false;
    for (;;) {
        uint32_t len = 0;
        if (adc_continuous_read(s_adc, buf, sizeof(buf), &len, 1000) != ESP_OK) {
            continue;
        }
        uint32_t edges = 0;
        uint16_t lo = UINT16_MAX;
        uint16_t hi = 0;
        for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= len; i += SOC_ADC_DIGI_RESULT_BYTES) {
            const adc_digi_output_data_t *p = (const adc_digi_output_data_t *)&buf[i];
            if (p->type1.channel != s_adc_channel) {
                continue;   // the S2's arbiter can hand back invalid samples
            }
            uint16_t v = p->type1.data;
            lo = MIN(lo, v);
            hi = MAX(hi, v);
            if (!low && v < CONFIG_DUMBLEDOOR_LOBBY_ADC_LOW) {
                low = true;
                edges++;
            } else if (low && v > CONFIG_DUMBLEDOOR_LOBBY_ADC_HIGH) {
                low = false;
                edges++;
            }
        }
        portENTER_CRITICAL(&s_mux);
        s_edges[LINE_LOBBY] += edges;
        s_adc_min = MIN(s_adc_min, lo);
        s_adc_max = MAX(s_adc_max, hi);
        portEXIT_CRITICAL(&s_mux);
        s_adc_low = low;
    }
}

static bool start_adc(void)
{
    adc_unit_t unit;
    if (adc_continuous_io_to_channel(CONFIG_DUMBLEDOOR_LOBBY_GPIO, &unit, &s_adc_channel) != ESP_OK ||
        unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d isn't an ADC1 pin (GPIO1–10), so the lobby input is read digitally instead",
                 CONFIG_DUMBLEDOOR_LOBBY_GPIO);
        return false;
    }
    adc_continuous_handle_cfg_t handle_cfg = { .max_store_buf_size = 1024, .conv_frame_size = ADC_FRAME_BYTES };
    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_12,   // the whole 0–3.3 V swing, like Arduino's analogRead()
        .channel = s_adc_channel,
        .unit = unit,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = ADC_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,   // 12-bit samples on the S2
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &s_adc));
    ESP_ERROR_CHECK(adc_continuous_config(s_adc, &cfg));
    ESP_ERROR_CHECK(adc_continuous_start(s_adc));
    xTaskCreate(adc_task, "lobby_adc", 3072, NULL, 7, NULL);
    ESP_LOGI(TAG, "lobby call line on GPIO%d, through the ADC (low below %d, high above %d of 4095)",
             CONFIG_DUMBLEDOOR_LOBBY_GPIO, CONFIG_DUMBLEDOOR_LOBBY_ADC_LOW, CONFIG_DUMBLEDOOR_LOBBY_ADC_HIGH);
    return true;
}
#endif

static void start_input(line_t line)
{
    int gpio = s_gpio[line];
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        // Pull towards "no call", so a loose wire can't ring.
        .pull_up_en = s_active[line] ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = s_active[line] ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    ESP_ERROR_CHECK(gpio_isr_handler_add(gpio, on_edge, (void *)(intptr_t)line));
    s_has_line[line] = true;
    ESP_LOGI(TAG, "%s call line on GPIO%d", line == LINE_LOBBY ? "lobby" : "front door", gpio);
}

// Two signals on one GPIO would mean the outputs drive something they shouldn't.
static bool pins_distinct(void)
{
    const int pins[] = {
        CONFIG_DUMBLEDOOR_LOBBY_GPIO,      CONFIG_DUMBLEDOOR_FRONT_GPIO,      CONFIG_DUMBLEDOOR_BELL_GPIO,
        CONFIG_DUMBLEDOOR_UNLOCK_GPIO,     CONFIG_DUMBLEDOOR_TALK_GPIO,       CONFIG_DUMBLEDOOR_BUTTON_GPIO,
        CONFIG_DUMBLEDOOR_LED_GPIO,        CONFIG_DUMBLEDOOR_PRESS_LED1_GPIO, CONFIG_DUMBLEDOOR_PRESS_LED2_GPIO,
        CONFIG_DUMBLEDOOR_PRESS_LED3_GPIO, CONFIG_DUMBLEDOOR_FAIL_LED_GPIO,
    };
    const size_t n = sizeof(pins) / sizeof(pins[0]);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (pins[i] >= 0 && pins[i] == pins[j]) {
                ESP_LOGE(TAG, "GPIO%d is set for two signals in menuconfig; leaving the outputs off", pins[i]);
                return false;
            }
        }
    }
    return true;
}

static void start_outputs(void)
{
    // Already at rest before they become outputs, so nothing twitches at boot.
    uint64_t outputs = 1ULL << CONFIG_DUMBLEDOOR_UNLOCK_GPIO;
    gpio_set_level(CONFIG_DUMBLEDOOR_UNLOCK_GPIO, !OUTPUT_ON);
#if CONFIG_DUMBLEDOOR_TALK_GPIO >= 0
    outputs |= 1ULL << CONFIG_DUMBLEDOOR_TALK_GPIO;
    gpio_set_level(CONFIG_DUMBLEDOOR_TALK_GPIO, !OUTPUT_ON);
#endif
#if CONFIG_DUMBLEDOOR_BELL_GPIO >= 0
    outputs |= 1ULL << CONFIG_DUMBLEDOOR_BELL_GPIO;
    gpio_set_level(CONFIG_DUMBLEDOOR_BELL_GPIO, BELL_LINE_IDLE);
#endif
    gpio_config_t out = { .pin_bit_mask = outputs, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&out));
}

void intercom_init(void)
{
    s_outputs_ok = pins_distinct() && PINS_CONFIRMED;
    if (!PINS_CONFIRMED) {
        ESP_LOGE(TAG, "GPIOs not confirmed (menuconfig → Dumbledoor → Intercom lines): only watching the "
                      "inputs; the door, the audio channel and the monitor's call line are left alone");
    }
    if (s_outputs_ok) {
        start_outputs();
    }

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE: already installed
        ESP_ERROR_CHECK(err);
    }
#if CONFIG_DUMBLEDOOR_LOBBY_ADC
    s_lobby_adc = start_adc();
    s_has_line[LINE_LOBBY] = s_lobby_adc;
#endif
    for (int line = 0; line < LINE_COUNT; line++) {
        if (s_gpio[line] >= 0 && !s_has_line[line]) {
            start_input(line);
        }
    }

    // Copying the tone to the monitor needs every edge as it happens, which only the
    // digital input gives.
    s_bell = s_outputs_ok && !s_lobby_adc;
#if CONFIG_DUMBLEDOOR_BELL_GPIO >= 0
    if (s_outputs_ok && s_lobby_adc) {
        ESP_LOGW(TAG, "bell_pin needs the lobby input read digitally; the monitor's call line is left at rest");
    }
#endif
}

bool intercom_has_line(line_t line)
{
    return s_has_line[line];
}

bool intercom_outputs_ok(void)
{
    return s_outputs_ok;
}

bool intercom_signal(line_t line)
{
    if (!s_has_line[line]) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    uint32_t edges = s_edges[line];
    s_edges[line] = 0;
#if CONFIG_DUMBLEDOOR_BELL_GPIO >= 0
    if (line == LINE_LOBBY && edges == 0 && s_bell) {
        gpio_set_level(CONFIG_DUMBLEDOOR_BELL_GPIO, BELL_LINE_IDLE);   // in case the last edge was missed
    }
#endif
    portEXIT_CRITICAL(&s_mux);
#if CONFIG_DUMBLEDOOR_LOBBY_ADC
    if (line == LINE_LOBBY && s_lobby_adc) {
        return edges >= MIN_EDGES || s_adc_low == (LOBBY_ACTIVE == 0);
    }
#endif
    return edges >= MIN_EDGES || gpio_get_level(s_gpio[line]) == s_active[line];
}

bool intercom_take_range(uint16_t *min, uint16_t *max)
{
#if CONFIG_DUMBLEDOOR_LOBBY_ADC
    if (!s_lobby_adc) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    *min = s_adc_min;
    *max = s_adc_max;
    s_adc_min = UINT16_MAX;
    s_adc_max = 0;
    portEXIT_CRITICAL(&s_mux);
    return *min <= *max;
#else
    (void)min;
    (void)max;
    return false;
#endif
}

void intercom_set_bell(bool on)
{
    if (!s_outputs_ok || s_lobby_adc) {
        return;
    }
    s_bell = on;
#if CONFIG_DUMBLEDOOR_BELL_GPIO >= 0
    if (!on) {
        portENTER_CRITICAL(&s_mux);
        gpio_set_level(CONFIG_DUMBLEDOOR_BELL_GPIO, BELL_LINE_IDLE);
        portEXIT_CRITICAL(&s_mux);
    }
#endif
}

bool intercom_has_bell(void)
{
    return CONFIG_DUMBLEDOOR_BELL_GPIO >= 0 && s_outputs_ok && !s_lobby_adc;
}

void intercom_set_unlock(bool on)
{
    if (s_outputs_ok) {
        gpio_set_level(CONFIG_DUMBLEDOOR_UNLOCK_GPIO, on ? OUTPUT_ON : !OUTPUT_ON);
    }
}

void intercom_set_talk(bool on)
{
#if CONFIG_DUMBLEDOOR_TALK_GPIO >= 0
    if (s_outputs_ok) {
        gpio_set_level(CONFIG_DUMBLEDOOR_TALK_GPIO, on ? OUTPUT_ON : !OUTPUT_ON);
    }
#else
    (void)on;
#endif
}

bool intercom_has_talk(void)
{
    return CONFIG_DUMBLEDOOR_TALK_GPIO >= 0;
}
