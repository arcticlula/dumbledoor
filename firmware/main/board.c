#include "driver/gpio.h"
#include "sdkconfig.h"
#include "board.h"

#if CONFIG_DUMBLEDOOR_LED_ACTIVE_HIGH
#define LED_ON 1
#else
#define LED_ON 0
#endif

// Green for the 1st, 2nd and 3rd press, then red; active high, as in the old sketches.
static const int s_debug_pins[4] = {
    CONFIG_DUMBLEDOOR_PRESS_LED1_GPIO,
    CONFIG_DUMBLEDOOR_PRESS_LED2_GPIO,
    CONFIG_DUMBLEDOOR_PRESS_LED3_GPIO,
    CONFIG_DUMBLEDOOR_FAIL_LED_GPIO,
};

static bool s_drive;
static uint8_t s_debug = 0xFF;   // last state written, bits as in s_debug_pins

void board_init(bool drive_leds)
{
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << CONFIG_DUMBLEDOOR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    s_drive = drive_leds;
    if (!s_drive) {
        return;
    }
    uint64_t outputs = 0;
#if CONFIG_DUMBLEDOOR_LED_GPIO >= 0
    gpio_set_level(CONFIG_DUMBLEDOOR_LED_GPIO, !LED_ON);
    outputs |= 1ULL << CONFIG_DUMBLEDOOR_LED_GPIO;
#endif
    for (int i = 0; i < 4; i++) {
        if (s_debug_pins[i] >= 0) {
            gpio_set_level(s_debug_pins[i], 0);
            outputs |= 1ULL << s_debug_pins[i];
        }
    }
    if (outputs) {
        gpio_config_t leds = { .pin_bit_mask = outputs, .mode = GPIO_MODE_OUTPUT };
        ESP_ERROR_CHECK(gpio_config(&leds));
    }
}

bool board_button_down(void)
{
    return gpio_get_level(CONFIG_DUMBLEDOOR_BUTTON_GPIO) == 0;
}

void board_led(bool on)
{
#if CONFIG_DUMBLEDOOR_LED_GPIO >= 0
    if (s_drive) {
        gpio_set_level(CONFIG_DUMBLEDOOR_LED_GPIO, on ? LED_ON : !LED_ON);
    }
#else
    (void)on;
#endif
}

void board_debug_leds(uint8_t greens, bool red)
{
    uint8_t state = (greens & 0x07) | (red ? 0x08 : 0);
    if (!s_drive || state == s_debug) {
        return;
    }
    s_debug = state;
    for (int i = 0; i < 4; i++) {
        if (s_debug_pins[i] >= 0) {
            gpio_set_level(s_debug_pins[i], (state >> i) & 1);
        }
    }
}
