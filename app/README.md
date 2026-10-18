# Dumbledoor app

The phone app: a web app with no build step and no dependencies, just ES modules. It exists in two places:

- **At home**, Dumbledoor serves it itself at `http://dumbledoor.local` (the files are built into the firmware). It talks to the device over its WebSocket, and works with no internet at all. Admin settings (phones, the code, remote access, firmware updates) are only available here.
- **Online**, a copy hosted on `https://` (GitHub Pages, or any static host) works from anywhere through the MQTT broker, and can be installed to the home screen. See [../docs/remote-access.md](../docs/remote-access.md).

## Using it

- **Hold the round button** for 0.7 s to open the lobby door. A tap does nothing, so a phone in a pocket can't open it. The button turns green when the door is released; if it can't, it says why.
- **When someone rings**, a banner shows at the top (with the phone vibrating), and stays for 2 minutes with "door opened" once it's been opened.
- **When someone rings** (the card): auto-open for 15 min to 3 h, and muting the monitor upstairs. The mute row only appears once the device passes calls through to the monitor (`bell_pin` connected).
- **Activity:** the last 20 rings, openings and changes, with how long each press was. That's how to calibrate the lobby code's long/short threshold.
- **Settings:** this phone's key and notifications. Admins at home also get the other phones (with pairing links), the lobby code, remote access, ring notifications, the device's name, and firmware updates.

## Files

| File | |
|---|---|
| `index.html`, `style.css` | the page |
| `main.js` | the screens: pairing, Wi-Fi setup, the door, activity, settings |
| `device.js` | the connection: signing commands, the WebSocket at home, the broker away |
| `crypto.js` | SHA-256 and HMAC-SHA256. Plain JS, because `crypto.subtle` isn't available on plain `http://` |
| `mqtt.js` | a small MQTT 3.1.1 client over WebSockets |
| `sw.js`, `manifest.webmanifest`, `icon*` | installing to the home screen, offline start, notifications |

`?remote` or `?local` at the end of the address picks the connection by hand; otherwise it's the home WebSocket on `http://` and the broker on `https://`.

## Trying it without the hardware

[`../tools/sim/sim.mjs`](../tools/sim/sim.mjs) is a pretend Dumbledoor that serves this folder and speaks the same protocol:

```sh
node tools/sim/sim.mjs                # then open http://localhost:8080
```

In its terminal, `r` / `l` ring the lobby briefly or long, `f` rings the front door, `c` enters the lobby code, `b` holds the board's button (so a phone can pair again), and `q` quits. Keys and settings are saved in `tools/sim/.sim-state-<port>.json`; `--fresh` starts over. To try the remote path:

```sh
node tools/sim/sim.mjs --port 8081 --mqtt wss://broker.hivemq.com:8884/mqtt \
  --prefix my-random-prefix --app-url 'http://localhost:8081/?remote'
```

Pair at `http://localhost:8081`, make a phone in Settings → Phones, and open its "online app" link in another browser profile: it connects through that public test broker.

## Tests

From the repository root (`cd tools && npm install` once, for Playwright):

```sh
node tools/test/crypto.test.mjs       # SHA-256/HMAC against Node's crypto
node tools/test/protocol.test.mjs     # signing, replay and permissions, against the simulator
node tools/test/mqtt.test.mjs         # the MQTT client against public brokers (needs internet)
cd tools && npm run e2e               # the whole app in a headless browser
```

## After changing the app

The home copy is built into the firmware: rebuild it (`idf.py build`) and flash, or upload it from Settings → Dumbledoor. The online copy updates when the change is pushed. Bump `CACHE` in `sw.js` when changing which files exist.
