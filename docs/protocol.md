# Protocol (version 1)

How the app talks to Dumbledoor. There are two ways to connect, with the same messages over both:

- **Home network:** the WebSocket at `ws://dumbledoor.local/ws`, plus `POST /api/cmd` for scripts.
- **Anywhere:** an MQTT broker that both the device and the phones connect out to (see [remote-access.md](remote-access.md)).

The firmware side is [`firmware/main/api.c`](../firmware/main/api.c) and [`keys.c`](../firmware/main/keys.c); the app side is [`app/device.js`](../app/device.js). [`tools/sim/sim.mjs`](../tools/sim/sim.mjs) implements the same protocol for testing.

## Keys and signed commands

Every phone has its own **key**: an 8-hex-digit id, a 32-byte random secret, a name, and an admin flag. The device creates keys (on pairing) and keeps up to 12 in flash.

A command is a JSON **envelope**:

```json
{ "k": "24e24a76", "p": "{\"d\":\"dd-1a2b3c\",\"t\":1792265608673,\"a\":\"open\"}", "s": "4a2e38…" }
```

- `k`: the key's id.
- `p`: the payload, a JSON object serialized to a string. `d` is the device id, `t` a timestamp in ms, `a` the action, and the action's arguments follow.
- `s`: HMAC-SHA256 of the UTF-8 bytes of `p`, keyed with the key's secret, as 64 hex digits.

The device accepts a command when all of these hold:

1. The key exists.
2. The signature matches (compared in constant time).
3. `d` is this device's id (`dd-` and the last three bytes of its MAC).
4. `t` is newer than the last accepted `t` from this key. This is kept in flash, so a recorded command can't be played back, even after a restart.
5. Once the device's clock is set over NTP, `t` is within 2 minutes of it.

The secret never travels after pairing, and nothing in transit can be replayed or altered.

```js
const p = JSON.stringify({ d: deviceId, t: Date.now(), a: 'open' });
const s = toHex(hmacSha256(secret, utf8(p)));
send(JSON.stringify({ k: keyId, p, s }));
```

The app keeps `t` strictly increasing per key (`max(now, last + 1)`).

Every command gets a **reply**: `{"type":"reply","re":"<first 16 hex digits of s>","ok":true, …}`, or `"ok":false` with an `"error"` code. `re` matches replies to commands, which matters on the broker, where every phone sees every reply.

## Pairing links

`<app address>#pair=<base64url of a JSON object>`, unpadded:

| Field | Meaning |
|---|---|
| `v` | protocol version, 1 |
| `d`, `dn` | device id and name |
| `k`, `s` | key id and secret (hex) |
| `n`, `a` | key name and admin flag |
| `m` | only for links made once remote access is set up: `u` the phones' broker address (`wss://…`), `user`, `pass`, `t` the topic prefix |

The part after `#` never leaves the phone, so the server hosting the app doesn't see it. The link is a key, though: whoever has it can open the door.

## Getting a key

- **The first admin:** `POST /api/claim` with `{"name": "…"}` returns `{"ok":true,"pair":{…}}`. It only works on the home network during a **claim window**: the first 30 minutes after power-up while the device has no keys, or 10 minutes after the board's button was held for 5 seconds. One phone per window.
- **Everyone else:** an admin creates a key with `key_add` and sends its link.

## Actions

| `a` | Arguments | Who | Reply has |
|---|---|---|---|
| `hello`, `state` | — | any key | `name`, `admin`, `state` (the short one unless an admin is at home) |
| `open` | — | any key | error `busy` during the 1 s pulse and the 3 s cooldown |
| `auto` | `min`: 0–720 (0 = off) | admin | — |
| `mute` | `min`: −1–1440 (0 = ring, −1 = until unmuted) | admin | — |
| `code` | optional `set`: fields to change (below) | admin, anywhere | `code` |
| `settings` | optional `set`: fields to change (below) | admin, home network | `settings` |
| `keys` | — | admin, home network | `keys`: `[{id, name, admin, last}]` |
| `key_add` | `name` (≤ 23 bytes), `admin` | admin, home network | `pair`, `localLink`, and `link` if the online app's address is set |
| `key_remove` | `id` | admin, home network | — (error `last_admin` for the only admin) |
| `reboot` | — | admin, home network | — |
| `ota` | — | admin, home network | only as the `X-Auth` header of `POST /api/ota` |

A phone from an ordinary link (a key that isn't admin) only opens the door: anything else is `forbidden`. Keys, settings and firmware are refused through the broker with `local_only`, even for admin keys. The lobby code is the exception: admins can change it from anywhere, and its presses travel sealed (below).

### The lobby code

`{"a":"code","set":{…}}` changes only the fields it includes; without `set` it just returns the code. The reply's `code` has all of them:

```json
{ "on": true, "seq": "<24 hex digits>", "short": 400, "long": 700, "gap": 1500 }
```

The code is up to 12 presses: `.` for a short one, `-` for a long one. Presses under `short` ms are short and presses of at least `long` ms are long (both 100–5000, `short` ≤ `long`). Anything in between counts as neither, so the code fails. `gap` (300–10000 ms) is the pause that ends a code. A code that's `on` needs at least one press.

`seq` is **sealed** both ways, so the presses never cross the broker in clear:

1. Pad the presses with zero bytes to 12 bytes.
2. XOR them with the first 12 bytes of HMAC-SHA256(the key's secret, `"code>" + t`) in a command, or `"code<" + t` in its reply. `t` is the command's timestamp, in decimal as in `p`.
3. Send the result as 24 hex digits.

Timestamps from a key never repeat, so no keystream is used twice, and the padding hides how long the code is. Only the phone that sent the command can open the reply. A `seq` that doesn't open to presses followed by padding is refused with `invalid_code`.

```js
const ks = hmacSha256(secret, utf8(`code>${t}`)); // t: the same timestamp as in p
const bytes = new Uint8Array(12);
bytes.set(utf8('..-'));
const seq = toHex(bytes.map((b, i) => b ^ ks[i]));
```

### Settings

`{"a":"settings","set":{…}}` changes only the fields it includes; without `set` it just returns them.

```json
{
  "name": "Dumbledoor",
  "mqtt": { "on": true, "url": "mqtts://…:8883", "user": "…", "pass": "…", "prefix": "dumbledoor",
            "appUrl": "wss://…:8884/mqtt", "appUser": "…", "appPass": "…" },
  "appUrl": "https://…/dumbledoor/",
  "ntfy": "https://ntfy.sh/…"
}
```

Passwords are never sent back: the reply has `hasPass` and `hasAppPass` instead, and an empty password in `set` keeps the current one. The lobby code isn't part of the settings: it has its own action, `code`.

## State

Admins get the full state. Other keys get the **short state**, just what opening the door needs: `type`, `v`, `id`, `name` and `pins`. So do replies through the broker, which every phone on it sees. How it reaches the phones is under WebSocket and MQTT below; `hello` and `state` replies include it too.

| Field | Meaning |
|---|---|
| `type`, `v` | `"state"`, protocol version |
| `id`, `name` | device id and name |
| `pins` | false until the GPIOs are confirmed in menuconfig: the door and the monitor's line are then left alone |
| `fw` | firmware version |
| `time`, `up` | device clock (Unix ms, 0 until NTP answers) and uptime (ms) |
| `lines` | wired call inputs: `"lobby"`, `"front"` |
| `ringing` | per line: someone is holding the button right now. Never the lobby while the lobby code is on (see Events). |
| `opening` | the door release is active |
| `auto` | seconds of auto-open left |
| `bell`, `mute` | whether calls pass through to the monitor; mute seconds left (−1 = until unmuted) |
| `code` | `on`, and `locked`: seconds code entry stays paused after 5 wrong codes |
| `remote` | `on` (configured) and `ok` (connected to the broker) |
| `wifi`, `heap` | network name and signal (dBm), free memory (bytes) |
| `events` | the last 20 events, oldest first |

## Events

Each has `n` (counts up from 1 at boot), `time` (Unix ms, 0 if the clock wasn't set yet), `up` (uptime ms) and `e`:

| `e` | Fields | When |
|---|---|---|
| `boot` | `reason`: power-on, restart, crash, watchdog, brownout, other | the device started |
| `ring` | `line`, `ms` (how long it was held) | a press ended |
| `open` | `by` (key name, `code`, `auto-open`), `via` (`local`, `remote`, `panel`, `auto`) | the door was released |
| `auto` | `by`, `min` | auto-open changed |
| `mute` | `by`, `min` | mute changed |
| `code_fail` | `count`, `seq` (the presses as read: `.` short, `-` long, `?` in between) | a wrong code at the panel |
| `denied` | `why`, `via` | a command with an unknown key or a bad signature (at most one per 10 s is listed) |
| `key_add`, `key_remove` | `name` | keys changed |

**While the lobby code is on**, nothing about lobby presses goes out while they last: no state push, no MQTT message. Their timing would give the code away. Once the code's pause (`gap`) has passed after the last press, Dumbledoor reports one outcome:

- the right code: `open` with `by: "code"`, and nothing about the presses, since they are the code;
- a wrong code: `code_fail` with its pattern;
- too few presses for a code (or code entry paused after wrong codes): a `ring` per press.

Auto-open waits for that moment too, and then opens the door, unless the code just did.

## Error codes

| Code | Meaning |
|---|---|
| `bad_request` | not a signed command |
| `unknown_key` | the key was removed (or never existed) |
| `bad_signature`, `wrong_device` | the signature doesn't match, or the key is for another device |
| `replay`, `clock` | the timestamp isn't newer than the last one, or is more than 2 minutes off the device's clock |
| `busy` | the door is opening or cooling down |
| `no_pins` | the firmware's GPIOs aren't confirmed, so it won't drive the door |
| `forbidden`, `local_only` | needs an admin key, or needs the home network |
| `invalid`, `invalid_code`, `invalid_mqtt`, `invalid_url` | a bad argument |
| `full`, `not_found`, `last_admin` | key management |
| `unknown_action`, `no_memory` | — |

## HTTP (home network)

| Request | Needs | |
|---|---|---|
| `GET /`, `/main.js`, … | — | the app, built into the firmware |
| `GET /api/info` | — | `{v, id, name, fw, host, setup, viaSetup, claim, paired}` |
| `GET /api/wifi` | — | Wi-Fi status, for the setup page |
| `POST /api/wifi` `{ssid, password}` | setup hotspot open | joins a network |
| `GET /api/scan` | setup hotspot open | nearby networks |
| `POST /api/claim` `{name}` | claim window | the first admin key |
| `POST /api/cmd` (envelope) | signed | the reply (HTTP 401 if the signature fails) |
| `POST /api/ota` (firmware .bin) | `X-Auth`: a signed `{"a":"ota"}` from an admin key | installs it and restarts |
| `GET /ws` | — | the WebSocket |

## WebSocket

The page sends envelopes as text frames and gets replies back. Once a command from a socket passes the signature check (the app starts with `hello`), the device pushes the state to that socket: the full state after every change for an admin key, and the short state only when it changes for the others. A push at every change would tell them each time someone rings. The device closes the socket if its key is removed.

## MQTT

Topics are `<prefix>/<device id>/…`, where the prefix is `dumbledoor` unless changed in the settings:

| Topic | Who publishes | |
|---|---|---|
| `…/status` | device | `online`, or `offline` as its last will. Retained. |
| `…/state` | device | the short state, retained, when it changes |
| `…/state/<key id>` | device | the full state for that admin key, sealed (below). Retained, after every change; emptied when the key is removed. |
| `…/cmd` | phones | envelopes; `hello`, `state` and `open` are accepted, and `auto`, `mute` and `code` from admin keys |
| `…/reply` | device | replies, not retained |

Every phone has the broker login and whoever runs the broker sees all of it, so anything there is readable by all of them. That's why the plain state is the short one, and why each admin's full state is **sealed** with that admin's key:

```json
{ "n": "<nonce: 16 hex digits>", "m": "<tag: 32 hex digits>", "x": "<base64>" }
```

- K = HMAC-SHA256(the key's secret, `"state<" + n`). The nonce is random, so every message has its own K.
- `x` is the state's JSON (UTF-8) XORed with a keystream made of 32-byte blocks HMAC-SHA256(K, `"k" + i`), for i = 0, 1, 2, …
- `m` is the first 16 bytes of HMAC-SHA256(K, `"m"` followed by the bytes of `x`). The app drops a message whose tag doesn't match.

Everything is QoS 0 apart from the status (QoS 1). The broker can't open the door, replay commands or change anything, and it can't read the activity. It still sees when messages go by: a sealed state goes out at every change, so its timing shows roughly when something happens. With the lobby code on, lobby presses only go out as one outcome once they stop (see Events), so that timing doesn't show the code's pattern.
