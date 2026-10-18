# Dumbledoor

*Dumb le door (no more).* An ESP32-S2 board that makes an old BPT System 200 intercom a little smarter, without modifying the intercom itself. It sits between the lobby panel's call line and the monitor in the apartment, and:

- opens the lobby door from a phone, at home or from anywhere;
- shows who rang and when (lobby and front door), with notifications;
- opens with a code of short and long presses on the lobby panel's call button;
- can open by itself for a while (when expecting guests), or keep the monitor quiet.

Build notes and photos: [Hackaday](https://hackaday.io/project/203886-dumbledoor) · [lulalab.pt](https://lulalab.pt/#/projects/dumb-le-door)

## How it works

```
 lobby panel ──call──► Dumbledoor ──call──► monitor upstairs
                       (ESP32-S2)  └──────► door release ("button" on terminal 9)
                          │   │
        home Wi-Fi ───────┘   └─────── MQTT broker on the internet
             │                                 │
   phone at home                      phone anywhere
   http://dumbledoor.local            the same app, hosted on https://
```

Dumbledoor listens to the call line from the panel and times every press, which is what makes press codes possible. To open the door, it "presses" the monitor's door button through a MOSFET and the right resistor. The schematic also routes the call line through the board, so the firmware can pass the call to the monitor or mute it. That part isn't connected yet: today the monitor rings straight from the panel.

Each phone has its own key and signs every command with it, so the secret never travels, and a recorded command can't be replayed. Keys can be removed one by one. The broker that relays commands from outside can't open the door by itself.

## What's here

| | |
|---|---|
| [`app/`](app/) | the phone app: a web app the device serves at home, and that can be hosted online ([README](app/README.md)) |
| [`firmware/`](firmware/) | ESP-IDF firmware for the ESP32-S2 ([README](firmware/README.md)) |
| [`dumbledoor/`](dumbledoor/) | the KiCad schematic |
| [`docs/hardware.md`](docs/hardware.md) | the intercom, the circuit, measured signals, pins, and what to double-check |
| [`docs/protocol.md`](docs/protocol.md) | how the app and the device talk, and how commands are signed |
| [`docs/remote-access.md`](docs/remote-access.md) | setting up the broker, the online app and push notifications |
| [`tools/`](tools/) | a simulator of the device, a command-line client, and tests |
| [`legacy/arduino/`](legacy/arduino/) | the Arduino sketches that ran on the board in September 2025, before this firmware |
| [`PLAN.md`](PLAN.md) | decisions, status, open questions, next steps |

## Getting started

1. Set the GPIOs to match the board and flash the firmware: [firmware/README.md](firmware/README.md).
2. Join the **Dumbledoor Setup** Wi-Fi with a phone and choose the home network.
3. Back on the home Wi-Fi, open `http://dumbledoor.local`, pair the phone (it becomes the admin), and add it to the home screen.
4. Optionally, set up remote access and ring notifications: [docs/remote-access.md](docs/remote-access.md).

To try the app without the hardware, run `node tools/sim/sim.mjs` and open `http://localhost:8080`.

## Status

The firmware builds (ESP-IDF 5.4.1, no warnings) and the app passes its tests against the simulator, at home and through a public broker. It hasn't run on the real board yet: see [PLAN.md](PLAN.md) → Next steps.
