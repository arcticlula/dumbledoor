# Dumbledoor — Plan

Last updated 2026-10-20. The firmware is written and builds with no warnings, set up for the board as wired: lobby input on GPIO1 (through the ADC), door on GPIO13, debug LEDs on 39/37/35/33, nothing else connected yet. The old Arduino code from September 2025 is recovered in `legacy/arduino`, and its thresholds are carried over. The app passes its tests end-to-end against a simulator of the device, both on the home network and through a public MQTT broker. On 2026-10-18 the firmware first ran on the board. It boots and sets up the pins, and the lobby input reads 4095 at rest (well above the 3000 threshold). The first Wi-Fi failure (reason 2) was a wrong password. With the right one it joins, but opening `http://dumbledoor.local` makes it reset, and afterwards it stays off the network. The core dump showed the cause. The ADC's DMA interrupt (the lobby input) ran while flash was being read, and called an ESP-IDF function that was still in flash, so it crashed. The board crashed again on most boots afterwards. `CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE` fixes this: with it flashed, the page loads and the board stays up. The same day the schematic was checked against BPT's System 200 manuals. Two parts of it shouldn't be built as drawn: the monitor's call output (Q3/R8) can't ring the monitor and holds its line at 12 V while the board is off, and the audio path would load line 9, which the whole building shares. [docs/hardware.md](docs/hardware.md) has the problems, a fail-safe replacement for the bell, and the measurements to make first. On 2026-10-19 admins got the lobby code in the online app too: it's the one setting that changes through the broker, and its presses travel sealed with the phone's key. The same day, phones from ordinary links were cut down to the open button: the device doesn't send them rings or the activity at all, and on the broker each admin gets the full state encrypted with their own key. On 2026-10-20 lobby presses stopped going out while the code is on: once they stop, only the outcome does (the opening, a wrong code with its pattern, or rings), so message timing can't give the code away either. To continue, start at **Next steps**.

## Goal

Open the building's lobby door from a phone, at home and from anywhere, using the apartment's existing BPT System 200 intercom and without modifying it. Keep the original ideas working too: detect calls, and open the door with a code of presses on the lobby panel's call button.

**Rule:** the intercom must keep working as before. The monitor rings, its own door button works, and Dumbledoor without Wi-Fi still passes calls through.

## Decisions

| Topic | Decision | Why |
|---|---|---|
| Firmware | ESP-IDF v5.4.1 (installed at `~/esp/esp-idf`), in C, laid out like the Mood Lamp's | Same toolchain and style as the other recent projects. The HTTP server, MQTT client, mDNS, NTP and OTA updates are all built in. |
| Phone ↔ device | Wi-Fi | The ESP32-S2 has no Bluetooth, so no Web Bluetooth like the Mood Light. |
| App | A web app with no build step, served by the device at home, with a copy on `https://` that installs as a PWA | Works on any phone with nothing from an app store. The home copy works with no internet. |
| At home | WebSocket to `http://dumbledoor.local` | Instant state pushes, and no cloud involved. |
| From anywhere | An MQTT broker both sides connect out to; EMQX Cloud Serverless's free tier suggested | No port forwarding or fixed IP. HiveMQ Cloud's free plan is closing (Dec 2026). |
| Security | A key per phone, HMAC-SHA256-signed commands, strictly increasing timestamps kept in flash, ±2 min against NTP time | Secrets never travel after pairing; the broker can't open the door; recorded commands can't be replayed; keys are removed one by one. |
| First admin | Claim window: 30 min after power-up with no keys, or 10 min after holding the board's button 5 s | Being at the board is what makes you admin; a lost admin phone can be replaced. |
| Ordinary links (keys that aren't admin) | Only the open button. The device sends them the short state (name, protocol version, pins) at home and on the broker, and auto-open and mute became admin-only. On the broker each admin gets the full state on its own topic, sealed with its key. | Asked for on 2026-10-19. The activity showed every press's length, so any phone, and the broker through the login every link carries, could read the lobby code's pattern and see who comes and goes. |
| Lobby presses while the code is on | Held back until the code's pause has passed, then one outcome: the opening for the right code (without the presses), a wrong code with its pattern, or rings when there were too few presses for a code. The lobby's `ringing` stays false, and auto-open waits too. | Asked for on 2026-10-20. Updates at every press went out on the broker, encrypted but on time, so anyone with the broker login could time the presses and read the pattern. Admins hear about lobby rings about 1.5 s later (the code's pause). |
| Admin through the broker | Only the lobby code (since 2026-10-19), with its presses sealed by the phone's key | Keys, settings and firmware only change from the home network. The code was wanted away from home too. Every phone has the broker login, so the presses are XORed with an HMAC of the phone's secret and the command's timestamp, and only the device and that phone can read them. |
| Call pass-through (once `bell_pin` is connected) | A GPIO interrupt (in IRAM) copies the TLP627's output to `bell_pin` edge by edge | The schematic cuts terminal 7, so the firmware must forward the call. Copying edges reproduces the tone exactly, whatever its frequency, and allows muting. Under review since 18 Oct: the proposed bell driver takes its tone from LEDC, and silent code entry needs replays (docs/hardware.md). |
| Lobby input | Through the ADC by default (20 kHz, software Schmitt trigger at 1000/3000 of 4095); as a digital pin with an interrupt as the alternative | The ADC is what the last sketch (18 Sep 2025) used on GPIO1, and it copes with a Darlington's low level (~0.7–1 V), which sits at the S2's digital threshold. The bell needs the digital input. |
| Ring detection | Edges counted every 10 ms; 20 ms of tone starts a press, 120 ms without ends it | The call is a ~570 Hz tone that the TLP627 follows cycle by cycle. |
| Lobby code | Short presses under 400 ms, long ones from 700 ms, anything in between fails the code; ends after 1.5 s of silence; 5 wrong codes in 15 min pause it for 15 min | The old Arduino sketches' tested thresholds. A clear failure beats a guess; the pause limits guessing. Off until set up. |
| Notifications | In the app (vibration, banner, web notifications while it's open) plus optional ntfy pushes | Web Push from the ESP32 would need a push service and VAPID signing. ntfy is a single HTTPS POST. |
| Firmware updates | Uploaded from the app (admin, home network), rolled back if the new one crashes within a minute | The board lives behind the monitor. |
| Testing without the board | `tools/sim`, a Node simulator of the device, plus Playwright tests | Going up and down to the lobby was the hard part of the first round. |
| Unconfirmed pins | Outputs stay off until a menuconfig box confirms the GPIOs, and if two signals share one. `sdkconfig.defaults` confirms the current wiring. | A wrong pin could hold the door release (`bell_pin` idles high). The box is the step to repeat when rewiring. |
| Debug LEDs | Green per press, all flashing on a right code, red on a wrong one, as in the old sketches | They're on the perfboard already, and show at the board what the decoder makes of the presses. |

## Hardware

See [docs/hardware.md](docs/hardware.md). In short:

- BPT System 200 monitor on a VKP/203 BB bracket; a LOLIN S2 Mini on a perfboard.
- In the schematic, terminal 7 (lobby call) runs through the board: TLP627 in, IRLZ24 out. The IRLZ24 output is to be replaced by a fail-safe relay and a push-pull driver (proposed, after measurements).
- Door and audio-channel buttons are AO3400As loading terminal 9 through R2 and R1.
- Front-door bell on terminal 20 through a divider, which a tone (likely) would overload.
- A DAC path into terminal 9 for audio later, not to be built as drawn: line 9 is shared by the whole building.
- **Wired today:** the lobby input (GPIO1), the door (GPIO13) and the debug LEDs. The bell, audio channel, front door and audio out aren't connected, so terminal 7 still runs straight to the monitor.

## Open questions

1. **The ADC thresholds:** do the lobby input's readings cross 1000 and 3000 (of 4095) during a press? `idf.py monitor` logs the range at rest and for every press.
2. **R1 and R2** (the audio-channel and door resistors on terminal 9) have no values in the schematic.
3. **Terminal 20** (before connecting it): BPT's diagrams feed it from the second call-tone generator through the landing button, so probably a tone of ~10–14 V. The divider is only safe up to ~5.5 V (see hardware.md, "Worth checking"). Measure it.
4. **The door button:** does the system take it any time, or only with the audio channel open? The old sketches opened with the door output alone, so probably any time.
5. **Power:** USB, or terminal 6 through a regulator?
6. **The 6.1 kHz signal** from the August 2025 video: which line was it?
7. **The monitor's call current** on terminal 7 during a call. BPT's specs suggest tens of mA. It sizes the bell driver.
8. **The call tone** over a long press: one frequency (~570 Hz), or two alternating?
9. **The panel with the monitor disconnected:** does it still send the tone, and does the camera come on? If not, the bell relay needs a dummy load.
10. **Line 9's levels:** DC at rest, with the handset picked up and with the door button pressed, and the size of speech on it. Needed for any audio.
11. ~~**Message timing on the broker**~~ Done 2026-10-20: with the code on, lobby presses only go out once they stop, as one outcome. What's left is when each outcome happens, and ntfy's push at the first press (once per 30 s), which together give a sequence's total length but not its pattern.

## Next steps

1. ~~**Flash.**~~ Done 2026-10-18; it boots.
2. ~~**Wi-Fi and the crashes.**~~ It was a wrong password, plus the ADC interrupt; both are fixed. If it resets again, see "When it resets" in [firmware/README.md](firmware/README.md).
3. **Check it on the board:**
   - ~~The log shows "lobby input at rest: between … and …".~~ 4095 to 4095, good.
   - Pressing the lobby button logs "lobby: held for … ms (input between … and …)", and a green debug LED lights per press.
   - Pair a phone (step 4), then `node tools/ddctl.mjs --link … open` or the app pulses GPIO13 and the door opens.
4. **Pair:** Wi-Fi setup through the hotspot, then pair the phone at `http://dumbledoor.local`.
5. **Calibrate the code:** press the lobby button short and long a few times, a couple of seconds apart, and read the lengths in Activity. Set the short and long thresholds around them, then choose a code of 4 or more presses and turn it on.
6. **Remote access:** ~~the app online~~ (done 2026-10-18: [github.com/arcticlula/dumbledoor](https://github.com/arcticlula/dumbledoor), app at https://arcticlula.github.io/dumbledoor/). Next: a broker (EMQX Serverless), the Remote access settings, then new pairing links ([docs/remote-access.md](docs/remote-access.md)).
7. **ntfy** for ring notifications with the app closed.
8. **Schematic:** add the ESP32 board with these pins, R1/R2's values, and the protection diode if it goes in; re-export `docs/images/schematic.svg`.
9. **The rest of the circuit, when it's connected:**
   - Now: a 1N4148 across the TLP627's LED. Without it the LED sees ~10 V reverse on every call (rated 5 V).
   - The bell, for silent code entry: measurements 1–4 in [docs/hardware.md](docs/hardware.md#ringing-the-monitor-from-the-board-proposed), then the relay and driver in place of Q3/R7/R8/D2, then the firmware (relay, LEDC tone, silent code entry, task watchdog).
   - `talk_pin`, and the front door through a second TLP627 stage after measuring terminal 20.
   - `audio_out_pin`: only after measuring line 9, and only the switched version in hardware.md.

## Later

- **Audio to the entrance** (from the original plan): after a correct code, open the audio channel and play a chime through `audio_out_pin`. The S2's DAC has a cosine generator for tones (`driver/dac_cosine.h`), or `dac_continuous` for samples.
- **Talking from the app:** two-way audio through the panel. A big step: audio over the network, plus the intercom's audio lines.
- **Video** (from the original plan): a project of its own.
- **Guest keys that expire** after a day or a week.
- **Home Assistant:** it already speaks MQTT, so discovery messages would be little extra.
