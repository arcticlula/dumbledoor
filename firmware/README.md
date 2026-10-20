# Dumbledoor firmware

ESP-IDF firmware for the ESP32-S2 board that sits between the BPT System 200 intercom's lobby call line and its monitor. It passes calls through to the monitor, times every press, opens the lobby door on command, and serves the phone app. Design decisions and the plan are in [../PLAN.md](../PLAN.md); the circuit and the intercom's signals are in [../docs/hardware.md](../docs/hardware.md); the app's protocol is in [../docs/protocol.md](../docs/protocol.md).

## Wiring

The defaults are the board as wired in October 2026, and `sdkconfig.defaults` confirms them, so a plain build is ready to flash:

| Schematic net | What it does | GPIO |
|---|---|---|
| `lobby_button_pin` | TLP627 output; low on each high half of the lobby call tone. Read through the ADC. | 1 |
| `unlock_pin` | Q2's gate: the door "button" | 13 |
| debug LEDs | green: one per press of a code; red: wrong code or unclear press | 39, 37, 35 (green), 33 (red) |
| status LED | the S2 Mini's onboard LED | 15 |
| BOOT button | hold 5 s: Wi-Fi setup, and lets a new admin phone pair | 0 |
| `bell_pin` | Q3's gate: drives the monitor's call line once terminal 7 is cut | not connected |
| `talk_pin` | Q1's gate: the audio-channel "button" | not connected |
| `front_door_button_pin` | terminal 20 through the divider | not connected |

To change the wiring, set the GPIOs in `idf.py menuconfig` → **Dumbledoor** (-1 = not connected). The option **These GPIOs (and the door ones) match the board** guards the outputs: while it's off, the firmware only watches the inputs and never drives the door, the audio channel, the monitor's call line or the LEDs, and the app says so. The firmware also leaves the outputs alone if two signals share a GPIO.

**The lobby input** is read through the ADC by default, like the 18 Sep 2025 sketch: low below 1000 and high above 3000 (of 4095). `idf.py monitor` shows whether those fit, with lines like these (the numbers are only an example):

```
door: lobby input at rest: between 4080 and 4095 of 4095
door: lobby: ringing
door: lobby: held for 240 ms (input between 160 and 4095 of 4095)
```

The readings during a press have to cross both thresholds. If they don't, adjust them in menuconfig. To connect `bell_pin`, switch the lobby input to "As a digital pin": only the digital input's interrupt can copy the tone to the monitor as it happens.

Also in menuconfig: the input polarities, the door pulse (1 s), an optional audio-channel lead before the pulse, the cooldown between openings (3 s), and how much silence ends a press (120 ms).

## Build and flash

```sh
cd firmware
. ~/esp/esp-idf/export.sh
idf.py build                                   # menuconfig first only if the wiring changed
idf.py -p /dev/cu.usbmodem* flash monitor      # Ctrl+] leaves the monitor
```

The S2 Mini has no USB-to-serial chip, so the first flash may need download mode: hold BOOT (GPIO0), tap RST, release BOOT, and run the flash command again. After that the firmware's USB console lets `idf.py flash` reset into the bootloader by itself.

Once it's installed, updates don't need USB: in the app, Settings → Dumbledoor → **Update the firmware**, and pick `build/dumbledoor.bin`. A new firmware that crashes before it has run for a minute is rolled back at the next reset. The web app is built into the firmware, so app changes reach the home copy this way too.

## First start

1. On first boot there's no Wi-Fi saved, so Dumbledoor opens a **Dumbledoor Setup** hotspot. Join it with a phone; the setup page pops up. Choose the home network.
2. Switch the phone back to the home Wi-Fi and open `http://dumbledoor.local`. Tap **Pair this phone** to become the admin. This works for 30 minutes after power-up while there's no admin, or for 10 minutes after holding the board's button for 5 seconds.
3. Add the page to the home screen. Other phones get keys from Settings → Phones; remote access is in [../docs/remote-access.md](../docs/remote-access.md).

## How it behaves

- **Lobby calls** show up in admins' apps at once. Every press is timed and listed in the activity, with its length. While the lobby code is on, presses only show up once they stop, as one outcome: the door opening for the right code, the pattern for a wrong one, or rings when there weren't enough presses for a code. That way the timing of the updates can't give the code away. Phones from ordinary links only get the open button: the firmware sends them nothing about rings or the activity.
- **Once `bell_pin` is connected** (and terminal 7 cut), calls pass through to the monitor edge by edge, so it rings as before. The app can then mute it for 1 h, 8 h or until unmuted, while phones still hear about rings; a restart unmutes.
- **The debug LEDs:** a green LED lights for each press (the third one stays on from the third press on). All three flash when a code is right; the red one flashes for a wrong code, blinks once for a press between short and long, and stays on while code entry is paused.
- **Auto-open** (15 min to 3 h) opens the door half a second after every lobby ring. With the lobby code on, it waits for the presses to stop (1.5 s by default).
- **Code at the lobby panel:** a pattern of short (under 400 ms) and long (700 ms or more) presses on the apartment's call button opens the door once the presses stop (1.5 s by default). A press in between counts as neither, so that code fails. The thresholds are the old Arduino sketches' and can be changed in the app. Off until enabled there. Five wrong codes (of 3 or more presses) within 15 minutes pause code entry for 15 minutes.
- **The front door bell** (terminal 20) only shows up in admins' apps and notifications.
- **Opening** holds `unlock_pin` for 1 s, optionally after opening the audio channel first, then waits 3 s before it can open again.
- **The button:** holding it for 5 s opens the setup hotspot (to change Wi-Fi) and lets one new admin phone pair for 10 minutes, e.g. after losing the only admin phone.
- **The LED:** on while the door opens; fast blink while someone rings; slow blink while the setup hotspot is open; a short blip every 2 s while Wi-Fi is down.
- **Without Wi-Fi** the monitor still rings and the code still works.
- **Push notifications** go out through ntfy, if a topic is set (see the remote access doc).

## From a computer

[`../tools/ddctl.mjs`](../tools/ddctl.mjs) sends signed commands with a pairing link, for testing the hardware without a phone:

```sh
node tools/ddctl.mjs --link 'http://dumbledoor.local/#pair=…' state
node tools/ddctl.mjs --link 'http://dumbledoor.local/#pair=…' open
```

## Tests

`sh test/run.sh` runs the host tests (the code decoder) with the computer's C compiler; no ESP-IDF needed.

## Notes

- With the digital lobby input, the call tone is copied to `bell_pin` from a GPIO interrupt that has to keep running while flash is written (every accepted command saves its timestamp), so it lives in IRAM, and `sdkconfig.defaults` puts `gpio_get_level()`/`gpio_set_level()` there too (`CONFIG_GPIO_CTRL_FUNC_IN_IRAM`).
- The ADC lobby input uses the ADC's continuous (DMA) mode at 20 kHz on ADC1, which works alongside Wi-Fi (ADC2 doesn't). Its DMA interrupt also runs while flash is busy, so `sdkconfig.defaults` sets `CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE`. Without it the board crashed at random, whenever the interrupt and a flash read met (for example, while Wi-Fi started or the page loaded).
- Logs and `idf.py monitor` use the board's native USB (`CONFIG_ESP_CONSOLE_USB_CDC`).
- The keys live in flash (NVS), which isn't encrypted: someone with the board in hand can read them. Physical access to the board means admin access anyway, through the button.
- The partition table has a 64 KB `coredump` partition at the end of flash (added 2026-10-18; nothing before it moved). A firmware update from the app doesn't change the partition table, so a board flashed before that date needs one `idf.py flash` over USB to get it.
- `idf.py -p /dev/cu.usbmodem* erase-flash flash` wipes the Wi-Fi, keys and settings. A plain `flash` keeps them, including Wi-Fi saved by any earlier firmware (Arduino sketches save it in the same place).

## When Wi-Fi won't connect

The log shows `could not connect (reason N, signal S dBm)` and retries, waiting up to a minute between tries. Dumbledoor keeps retrying the saved network and doesn't open the hotspot by itself, so to change the network or password, hold the button for 5 s.

| Log | Meaning | What to do |
|---|---|---|
| `assoc -> run`, then `run -> init (0x2c0)` and reason 2 about 3 s later | The router accepted the board, then dropped it during the WPA2 handshake. This is almost always a wrong password. | Hold the button 5 s and enter the password again. |
| Reason 205 right after a reason 2 | The retry that follows; it isn't a separate problem. | As above. |
| Reason 2, 15 or 204 with the right password and a signal below about −75 dBm, or failures that come and go | A weak link. The S2 Mini's antenna connects better at lower power. | Set `idf.py menuconfig` → Dumbledoor → Max Wi-Fi transmit power to 34 (8.5 dBm), then flash. |
| Reason 201 | The network isn't in range, or it's 5 GHz only. The S2 only does 2.4 GHz. | Choose the 2.4 GHz network. |

In the `state:` lines, the hex code's last two digits are the frame that caused the change: `c0` is a deauthentication from the router, and the digits before it are the router's reason.

## When it resets

When the board resets, its USB port disappears with it (`read failed: [Errno 6] Device not configured`), so the crash message usually never reaches the monitor, and a brownout message never does. Two things are left to go on:

- **Five seconds after it starts**, the log says why: `started after: crash`, `watchdog`, `brownout`, `power-on` or `restart`. Starting `idf.py monitor` resets the board itself, so read this line from the monitor that was already open.
- **After a crash or a watchdog reset**, the board keeps a core dump in flash. Leave the monitor (Ctrl+]) and run `idf.py coredump-info` to get the task and the backtrace. If the board keeps crashing, `coredump-info` can fail with `stream read less than specified amount`. It reads the dump in two passes and resets the board in between, so a new crash replaces the dump halfway through. In that case, copy the partition while the board stays in the bootloader, then decode the copy:

  ```sh
  python -m esptool --chip esp32s2 -p /dev/cu.usbmodem* --after no_reset read_flash 0x3e0000 0x10000 core.bin
  python -m esp_coredump info_corefile --core core.bin --core-format raw build/dumbledoor.elf
  ```

A brownout means the 3.3 V supply dipped. That usually happens during Wi-Fi transmit bursts, with a weak USB port or cable. A transmit power of 34 (see the Wi-Fi table above) draws less current.
