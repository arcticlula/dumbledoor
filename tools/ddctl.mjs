#!/usr/bin/env node
// Sends signed commands to Dumbledoor from a computer on the home network, for testing
// the hardware without a phone. It uses a pairing link like any phone; make one in the
// app (Settings → Phones) and keep it private: it opens the door.
//
//   node tools/ddctl.mjs --link 'http://dumbledoor.local/#pair=…' state
//   node tools/ddctl.mjs --link "$(cat ~/.dumbledoor-link)" open
//   node tools/ddctl.mjs --link … auto 15        (open on every ring for 15 min; 0 = off)
//   node tools/ddctl.mjs --link … mute 60        (keep the monitor quiet for 60 min; -1 = until unmuted)
//   node tools/ddctl.mjs --link … keys | settings | reboot
//   node tools/ddctl.mjs --link … --host 192.168.1.50 open    (if .local names don't resolve)

import { fromBase64Url } from '../app/crypto.js';
import { sign } from '../app/device.js';

const args = process.argv.slice(2);
const take = (name) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args.splice(i, 2)[1] : undefined;
};
const link = take('link') || process.env.DUMBLEDOOR_LINK;
const hostArg = take('host');
const [action = 'state', value] = args;

if (!link) {
  console.error('Give a pairing link with --link or $DUMBLEDOOR_LINK. See the top of this file.');
  process.exit(2);
}
const match = link.match(/#pair=([\w-]+)/);
if (!match) {
  console.error('That doesn’t look like a pairing link (no #pair=…).');
  process.exit(2);
}
const pairing = JSON.parse(fromBase64Url(match[1]));
const host = hostArg || new URL(link).host;
// Links for the online app point at its host; commands still go straight to the device.
const base = hostArg || !new URL(link).protocol.startsWith('http:') ? `http://${hostArg || 'dumbledoor.local'}` : `http://${host}`;

const extra = {};
if (action === 'auto' || action === 'mute') extra.min = Number(value ?? 0);
const { text } = sign(pairing, action, extra);
const res = await fetch(`${base}/api/cmd`, { method: 'POST', body: text }).catch((err) => {
  console.error(`Can’t reach ${base}: ${err.cause?.code || err.message}`);
  process.exit(1);
});
const reply = await res.json();
if (action === 'state' && reply.state) {
  const s = reply.state;
  console.log(`${s.name} (${s.id}), firmware ${s.fw}, Wi-Fi ${s.wifi.ssid} ${s.wifi.rssi} dBm, free heap ${s.heap} B`);
  console.log(`ringing ${JSON.stringify(s.ringing)} · opening ${s.opening} · auto ${s.auto} s · mute ${s.mute} · code ${JSON.stringify(s.code)} · remote ${JSON.stringify(s.remote)}`);
  for (const e of s.events) console.log(' ', new Date(e.time || Date.now()).toLocaleTimeString(), JSON.stringify(e));
} else {
  console.log(JSON.stringify(reply, null, 2));
}
process.exit(reply.ok ? 0 : 1);
