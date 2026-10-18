#pragma once

#include <stdbool.h>
#include <stdint.h>

// The ESP32-S2 board's own button and LED, and the debug LEDs on the perfboard.
// The LEDs are only driven when `drive_leds`, i.e. once the GPIOs are confirmed.
void board_init(bool drive_leds);
bool board_button_down(void);
void board_led(bool on);

// The debug LEDs from the old sketches. `greens` is a bit mask, bit 0 = the first press's LED.
void board_debug_leds(uint8_t greens, bool red);
