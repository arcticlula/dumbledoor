# Old Arduino sketches (September 2025)

The code that ran on the board before the ESP-IDF firmware in [`../../firmware/`](../../firmware/), which replaces it. These sketches only do the lobby code: no app, no network, no passing the call to the monitor. They're kept as a record and for their tested values.

They were recovered from the Arduino IDE's build caches on the old PC (the `sketches` folder on the USB drive). Each build folder keeps the preprocessed sketch (`sketch/*.ino.cpp`), which was turned back into the original `.ino` using its `#line` markers. All three still compile for the LOLIN S2 Mini (esp32 core 3.3.7).

## What they have in common

- Board: **LOLIN S2 Mini** (`esp32:esp32:lolin_s2_mini`).
- Door: **GPIO13**, high for 1 s.
- A code of 3 presses: **short under 400 ms, long from 700 ms**. Presses in between are "invalid", and 1.5 s without a press resets the entry.

## The three versions

| Sketch | Built | How it reads the lobby call | Code |
|---|---|---|---|
| [`intercom_2025_09_03`](intercom_2025_09_03/intercom_2025_09_03.ino) | 3 Sep 2025, the day of the perfboard photo | GPIO12 (external pull-up). An interrupt on every falling edge; a press is a burst of pulses, over after 100 ms without one. | short, short, short |
| [`intercom_2025_09_15`](intercom_2025_09_15/intercom_2025_09_15.ino) | 15 Sep | GPIO12 with the internal pull-up, polled as a level: low for at least 20 ms is a press. Adds feedback LEDs: green on GPIO39, 37 and 35 (one per press), red on GPIO33 (wrong code). | short, long, short |
| [`intercom_2025_09_18`](intercom_2025_09_18/intercom_2025_09_18.ino) | 18 Sep, the last build | GPIO1 through the ADC, with a software Schmitt trigger (below 1000 / above 3000 of 4095) counting falling edges in bursts, like 3 Sep. Same LEDs. | short, short, short |

The board is still wired as in the last one: lobby input on GPIO1, door on GPIO13, LEDs on 39/37/35/33. The firmware reads GPIO1 through the ADC the same way (same thresholds) and drives the LEDs the same way.

Polling the level (15 Sep) suits a push button, but not the call itself: the optocoupler's output follows every cycle of the ~570 Hz call tone ([docs/hardware.md](../../docs/hardware.md)), so a "press" would end within a millisecond. Counting bursts of edges (3 and 18 Sep) handles the tone. The firmware does the same, and keeps these sketches' thresholds, including treating in-between presses as neither short nor long.
