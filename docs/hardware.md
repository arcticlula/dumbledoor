# Hardware

What's known about the intercom, how Dumbledoor connects to it, and what still needs checking. The circuit is the KiCad project in [`../dumbledoor/`](../dumbledoor/); the build photos and videos are on [lulalab.pt](https://lulalab.pt/#/projects/dumb-le-door) and [Hackaday](https://hackaday.io/project/203886-dumbledoor).

## The intercom

- **Monitor:** BPT System 200 video monitor with handset (Italian, from the 1990s).
- **Wall bracket:** BPT VKP/203 BB (part 62819300). The monitor plugs into it; the building's wires land on its screw terminals, numbered 3 to 20.
- **Lobby panel:** at the building's entrance, one call button per apartment. Each apartment has its own call wire.

### Terminals in use

From the labels on the wall next to the monitor:

| Terminal | Wire | Carries |
|---|---|---|
| 5 | black | GND |
| 6 | red | supply, about 12.5 V |
| 7 | green | call from the lobby panel |
| 8 | purple | audio, lobby → apartment |
| 9 | blue | audio, apartment → lobby. The monitor's audio and door buttons also work by loading this line. |
| 20 | yellow | call from the apartment's own doorbell, on the landing |

## How Dumbledoor connects

```
                     terminal 7 is cut here
 lobby panel ──7_in──►┐                        ┌──7_out──► monitor (rings)
                      │      Dumbledoor        │
                      ├─ TLP627 ─► lobby_button_pin    bell_pin ─► Q3 ─┤
                      │                        │
 terminal 9  ◄────────┼─ R2 ◄─ Q2 ◄─ unlock_pin  (door)                 │
             ◄────────┼─ R1 ◄─ Q1 ◄─ talk_pin    (audio channel)        │
             ◄────────┼─ R12/C1/R11 ◄─ audio_out_pin (DAC, not used yet)│
 terminal 20 ─────────┼─ R9/R10 ─► front_door_button_pin                │
 terminal 6 (12.5 V) ─┼────────────────────────── R8 ──────────────────┘
 terminal 5 (GND) ────┴─ common ground
```

In the schematic, the lobby call line (terminal 7) runs **through** Dumbledoor: the panel's side comes in as `intercomm_pin_7_in`, and Dumbledoor drives the monitor's side, `intercomm_pin_7_out`. The firmware copies the call tone to the monitor edge by edge, so the monitor rings as it always did, and the call can also be muted. The other lines are tapped in parallel.

### The board today (October 2026)

Only part of the schematic is connected:

- **Lobby call input** on GPIO1, read through the ADC.
- **Door release** on GPIO13.
- **Four debug LEDs** on GPIO39, 37 and 35 (green) and GPIO33 (red).

`bell_pin`, `talk_pin`, `audio_out_pin` and the front-door input aren't connected yet. With no `bell_pin`, terminal 7 still runs straight from the panel to the monitor, and Dumbledoor only listens to it: the monitor rings on its own, and the app can't mute it.

## The circuit

![Schematic](images/schematic.svg)

The net labels (`lobby_button_pin`, `bell_pin`, …) are what the firmware's menuconfig options refer to.

1. **Lobby call in.** `7_in` → C2 (1 µF, AC coupling) → R13 (1 kΩ) → the LED of U2, a TLP627 Darlington optocoupler. R5 (10 kΩ) gives C2 a path to GND. The TLP627's collector is `lobby_button_pin`, pulled up to 3.3 V by R6 (10 kΩ). The pin is high when idle and goes low on every high half-cycle of the tone, so it toggles for as long as the button is held.
2. **Monitor call out.** R8 (2.2 kΩ) pulls `7_out` up to terminal 6 (12.5 V), and D2 (1N4742A, 12 V Zener) clamps it. Q3 (IRLZ24) pulls it to GND while `bell_pin` is high. R7 (10 kΩ) keeps Q3 off until the ESP32 drives the pin. So `bell_pin` high means the line is at 0 V, its idle level, and `bell_pin` low means the line is at ~12 V.
3. **Door and audio-channel "buttons".** Q2 (AO3400A, gate `unlock_pin`) pulls terminal 9 to GND through R2; Q1 (AO3400A, gate `talk_pin`) through R1. These do what the monitor's two buttons do. **R1 and R2 have no values in the schematic yet**: they're the values found with the trimmer pot. R3 and R4 (100 kΩ) keep both MOSFETs off while the ESP32 boots.
4. **Audio into the panel.** `audio_out_pin` → R11 (100 Ω) → C1 (47 µF) → R12 (330 Ω) → terminal 9. It's meant for the ESP32-S2's DAC (GPIO17 or GPIO18). The firmware doesn't use it yet.
5. **Front door call in.** Terminal 20 → R9 (2.2 kΩ) / R10 (3.3 kΩ) divider → `front_door_button_pin`. That's 0.6 × the line's voltage, high while the doorbell is pressed.

Everything shares the intercom's ground (terminal 5): the TLP627 shifts the call's levels but doesn't isolate the grounds.

To update the image after editing the schematic:

```sh
kicad-cli sch export svg --exclude-drawing-sheet -o docs/images dumbledoor/dumbledoor.kicad_sch
mv docs/images/dumbledoor.svg docs/images/schematic.svg
```

## Signals

Scope captures from 19 Sep 2025. Hantek DSO5102P, 40 px per division; the frequencies were measured from the pixels. Which probe was on which line is my reading of the captures; correct it if it's wrong.

| Capture | What it shows |
|---|---|
| ![](images/scope-119_1.png) | 800 µs/div. **Yellow** (5 V/div): the lobby call on terminal 7, a square wave from ~0 V to roughly 9–11 V. **Cyan** (1 V/div): the TLP627 output, 3.3 V idle, pulled to 0 V on every high half of the tone. |
| ![](images/scope-119_3.png) | 2 ms/div: the start of a call. The tone's period is **~1.75 ms, about 570 Hz**. |
| ![](images/scope-119_2.png) | 20 ms/div: a whole short press. The tone lasts **~170 ms**, so its length follows the button, which is what makes press codes possible. The first cycles are the strongest. |
| ![](images/scope-120_1.png) | 8 ms/div, 5 V/div: a square wave from ~0 V to ~12 V. That's the regenerated call on `7_out`: the panel's own tone only reaches ~9–11 V. |
| ![](images/scope-120_2.png) | 8 ms/div, 2 V/div: a 3.3 V logic signal, idle high, toggling at the same rate. `bell_pin` (or the TLP627 output, which it copies). |

A video from 27 Aug 2025 measures a different line: a 6.10 kHz square wave, 0–8.2 V, with 76 µs pulses. It isn't the call (which is ~570 Hz), and which line it is isn't recorded.

In firmware terms, that means:

- The lobby input sees ~1,150 edges a second during a call. The firmware can read it two ways (`idf.py menuconfig` → Dumbledoor → "Read the lobby input"):
  - **Through the ADC** (the default, as the 18 Sep 2025 sketch did). 20,000 samples a second go through a software Schmitt trigger: below 1000 of 4095 is low, above 3000 is high, and every crossing counts as an edge. This tolerates a low level that isn't quite 0 V; a Darlington like the TLP627 can't pull all the way down (its saturation voltage is ~0.7–1 V). It only works on GPIO1–10 (ADC1). `idf.py monitor` logs the lowest and highest reading at rest and during every press, so the thresholds can be checked: the readings should cross both.
  - **As a digital pin**, with an interrupt on both edges. This is what `bell_pin` needs, because the interrupt also copies each edge to the monitor's line as it happens.
- Every 10 ms the door task checks whether any edges came in, which decides whether a press is in progress.
- A press starts after 20 ms of tone and ends after 120 ms without it (`DUMBLEDOOR_RING_RELEASE_MS`; the old sketches used 100 ms). The app's activity list shows every press's length, which is how to calibrate the code's short and long thresholds.

## Pins

The board as wired in October 2026. These are the firmware's defaults, and `firmware/sdkconfig.defaults` confirms them (`DUMBLEDOOR_PINS_SET`). Before rewiring, change them in `idf.py menuconfig` → Dumbledoor: while the confirmation is off, the firmware drives none of the outputs. The schematic doesn't include the ESP32 board yet, so this table is the record.

| Net | Direction | GPIO | menuconfig option |
|---|---|---|---|
| `lobby_button_pin` | in, active low, through the ADC | 1 | `DUMBLEDOOR_LOBBY_GPIO`, "Read the lobby input" |
| `unlock_pin` | out, active high | 13 | `DUMBLEDOOR_UNLOCK_GPIO` |
| debug LEDs | out, active high | 39, 37, 35 (green: 1st, 2nd, 3rd press), 33 (red) | `DUMBLEDOOR_PRESS_LED1..3_GPIO`, `DUMBLEDOOR_FAIL_LED_GPIO` |
| status LED | out | 15 (the S2 Mini's onboard LED) | `DUMBLEDOOR_LED_GPIO` |
| board button | in, to GND | 0 (BOOT on the S2 Mini) | `DUMBLEDOOR_BUTTON_GPIO` |
| `bell_pin` | out, high = line at rest | not connected | `DUMBLEDOOR_BELL_GPIO` (needs the lobby input read digitally) |
| `talk_pin` | out, active high | not connected | `DUMBLEDOOR_TALK_GPIO` |
| `front_door_button_pin` | in, active high | not connected | `DUMBLEDOOR_FRONT_GPIO` |
| `audio_out_pin` | DAC out | not connected (GPIO17 or 18 when it is) | — |

The old Arduino sketches ([`../legacy/arduino/`](../legacy/arduino/)) used the same lobby input, door and LEDs, apart from an earlier lobby input on GPIO12.

On the ESP32-S2, avoid GPIO 0, 45 and 46 for the intercom lines (strapping pins; 46 is input-only), 19 and 20 (USB, used for flashing and logs), and 26–32 on modules where they go to flash or PSRAM. The board is a LOLIN S2 Mini (4 MB flash, 2 MB PSRAM, native USB), which the old sketches were built for.

## Worth checking

1. **Once terminal 7 is cut, the monitor can't ring while Dumbledoor is off.** A dead board, a crash loop or a power cut would silence it. The firmware has watchdogs and keeps working without Wi-Fi, but a jumper or switch that reconnects `7_in` to `7_out` would be a good fallback. A normally-closed relay contact would do the same automatically. Not an issue today, while terminal 7 isn't cut.
2. **The TLP627's LED reverse voltage.** With C2 in series, the LED sees the negative half of the AC-coupled tone, up to roughly half the tone's swing (~−5 V for a 10 V tone). The TLP627's LED is rated for 5 V reverse. Until the 14 Sep 2025 revision the input was DC-coupled through a series diode (D1), which kept reverse voltage off the LED; the AC-coupled input from 25 Sep has nothing doing that. A 1N4148 across the LED (anti-parallel: its cathode on the LED's anode) would protect it.
3. **The front-door divider's headroom.** 0.6 × the line means anything above ~5.5 V on terminal 20 puts more than 3.3 V on the GPIO (the S2's absolute maximum is 3.6 V). If terminal 20 carries a tone like terminal 7's (~9–12 V), use a second TLP627 stage like the lobby's, or a 10 kΩ / 3.3 kΩ divider (0.25 ×).
4. **`7_out` while the ESP32 boots** (once `bell_pin` is connected). R7 holds Q3 off, so `7_out` sits at ~12 V DC for the second or so before the firmware drives `bell_pin` high (the idle level). The monitor rings on a tone, so this is probably harmless; worth confirming once.
5. **Power.** How the board is powered (USB, or terminal 6 through a regulator) isn't in the schematic yet.
