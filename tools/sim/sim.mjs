#!/usr/bin/env node
// A pretend Dumbledoor for trying the app without the hardware. It serves ../../app and
// speaks the same protocol as the firmware (docs/protocol.md): the WebSocket for the home
// network, and optionally MQTT through a broker for the remote path.
//
//   node tools/sim/sim.mjs                      http://localhost:8080
//   node tools/sim/sim.mjs --mqtt wss://broker.hivemq.com:8884/mqtt --prefix my-secret-prefix
//
// Keys (in a terminal): r short lobby ring · l long ring · f front door · c the lobby code
//                       b hold the board's button (lets a phone pair) · q quit
// Or over HTTP: POST /sim/ring?line=lobby&ms=300 · POST /sim/code · POST /sim/button

import { createHash, createHmac, randomBytes, timingSafeEqual } from 'node:crypto';
import { createServer } from 'node:http';
import { readFile, writeFile } from 'node:fs/promises';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { MqttClient } from '../../app/mqtt.js';

const here = dirname(fileURLToPath(import.meta.url));
const appDir = join(here, '..', '..', 'app');
const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : fallback;
};
const port = Number(opt('port', 8080));
const stateFile = join(here, `.sim-state-${port}.json`); // keys and settings survive restarts
const mqttUrl = opt('mqtt', '');
const fresh = args.includes('--fresh');

const VERSION = 1;
const ID = 'dd-5e1a7e';
const PULSE_MS = 1000;
const COOLDOWN_MS = 3000;
const MAX_SKEW_MS = 2 * 60 * 1000;
const bootAt = Date.now();
const uptime = () => Date.now() - bootAt;

// What the firmware keeps in flash.
let saved = {
  keys: [], // { id, name, admin, secret (hex), last }
  settings: {
    name: 'Dumbledoor',
    code: { on: false, seq: '..-', short: 400, long: 700, gap: 1500 },
    mqtt: { on: !!mqttUrl, url: mqttUrl.replace(/^wss/, 'mqtts'), user: '', pass: '', prefix: opt('prefix', 'dumbledoor'), appUrl: mqttUrl, appUser: '', appPass: '' },
    appUrl: opt('app-url', ''),
    ntfy: '',
  },
};
if (!fresh) {
  try {
    saved = JSON.parse(await readFile(stateFile, 'utf8'));
  } catch {}
}
const save = () => writeFile(stateFile, JSON.stringify(saved, null, 2)).catch(() => {});

// What it keeps in RAM.
const live = {
  ringing: { lobby: false, front: false },
  opening: false,
  openedAt: -Infinity,
  autoUntil: 0,
  mute: 0, // 0 off, -1 until unmuted, else Date.now() deadline
  claimUntil: saved.keys.length ? 0 : Date.now() + 30 * 60 * 1000,
  events: [],
  nextEvent: 1,
  code: { seq: '', last: 0, pressing: false, timer: null },
  fails: 0,
};

const log = (...parts) => console.log(new Date().toLocaleTimeString(), ...parts);

function post(event) {
  live.events.push({ n: live.nextEvent++, time: Date.now(), up: uptime(), ...event });
  if (live.events.length > 20) live.events.shift();
  changed();
}

function state() {
  const now = Date.now();
  return {
    type: 'state',
    v: VERSION,
    id: ID,
    name: saved.settings.name,
    fw: '0.1.0-sim',
    time: now,
    up: uptime(),
    pins: !args.includes('--no-pins'), // like firmware built without the GPIOs confirmed
    lines: ['lobby', 'front'],
    ringing: { ...live.ringing },
    opening: live.opening,
    auto: live.autoUntil > now ? Math.ceil((live.autoUntil - now) / 1000) : 0,
    bell: !args.includes('--no-pins'),
    mute: live.mute === -1 ? -1 : live.mute > now ? Math.ceil((live.mute - now) / 1000) : 0,
    code: { on: saved.settings.code.on, locked: 0 },
    remote: { on: !!saved.settings.mqtt.on, ok: !!remote?.connected },
    wifi: { ssid: 'Simulated Wi-Fi', rssi: -58 },
    heap: 123456,
    events: live.events,
  };
}

// Keys ------------------------------------------------------------------------

function pairing(key) {
  const s = saved.settings;
  const p = { v: VERSION, d: ID, dn: s.name, k: key.id, s: key.secret, n: key.name, a: key.admin };
  if (s.mqtt.on && s.mqtt.appUrl) {
    const own = !!s.mqtt.appUser;
    p.m = { u: s.mqtt.appUrl, user: own ? s.mqtt.appUser : s.mqtt.user, pass: own ? s.mqtt.appPass : s.mqtt.pass, t: s.mqtt.prefix };
  }
  return p;
}

const link = (base, p) => `${base}#pair=${Buffer.from(JSON.stringify(p)).toString('base64url')}`;

function addKey(name, admin) {
  if (saved.keys.length >= 12) return null;
  const key = { id: randomBytes(4).toString('hex'), name: name || 'Phone', admin, secret: randomBytes(32).toString('hex'), last: 0 };
  saved.keys.push(key);
  save();
  post({ e: 'key_add', name: key.name });
  return key;
}

// The same checks as keys_verify() in firmware/main/keys.c.
function verify(text) {
  let env;
  try {
    env = JSON.parse(text);
  } catch {
    return { error: 'bad_request' };
  }
  if (typeof env.k !== 'string' || typeof env.p !== 'string' || !/^[0-9a-f]{64}$/i.test(env.s || '')) return { error: 'bad_request' };
  const re = env.s.slice(0, 16);
  const key = saved.keys.find((k) => k.id === env.k);
  if (!key) return { error: 'unknown_key', re };
  const mac = createHmac('sha256', Buffer.from(key.secret, 'hex')).update(env.p).digest();
  if (!timingSafeEqual(mac, Buffer.from(env.s, 'hex'))) return { error: 'bad_signature', re };
  const p = JSON.parse(env.p);
  if (typeof p.t !== 'number') return { error: 'bad_request', re };
  if (p.d !== ID) return { error: 'wrong_device', re };
  if (p.t <= key.last) return { error: 'replay', re };
  if (Math.abs(p.t - Date.now()) > MAX_SKEW_MS) return { error: 'clock', re };
  key.last = p.t;
  save();
  return { re, p, key };
}

// Commands --------------------------------------------------------------------

function open(who, via) {
  const now = Date.now();
  if (live.opening || now - live.openedAt < COOLDOWN_MS) return false;
  live.openedAt = now;
  live.opening = true;
  log(`🚪 door released for ${who} (${via})`);
  post({ e: 'open', by: who, via });
  setTimeout(() => {
    live.opening = false;
    changed();
  }, PULSE_MS);
  return true;
}

function applySettings(set) {
  const s = structuredClone(saved.settings);
  for (const k of ['name', 'appUrl', 'ntfy']) if (typeof set[k] === 'string') s[k] = set[k];
  if (set.code) Object.assign(s.code, set.code);
  if (set.mqtt) {
    for (const [k, v] of Object.entries(set.mqtt)) if (!((k === 'pass' || k === 'appPass') && v === '')) s.mqtt[k] = v;
  }
  if (!/^[.-]*$/.test(s.code.seq) || (s.code.on && !s.code.seq) || s.code.short > s.code.long) return 'invalid_code';
  if (s.mqtt.on && !s.mqtt.url) return 'invalid_mqtt';
  const mqttChanged = JSON.stringify(s.mqtt) !== JSON.stringify(saved.settings.mqtt);
  saved.settings = s;
  save();
  if (mqttChanged) log('(remote access settings changed; restart the simulator to use them)');
  return null;
}

function settingsView() {
  const s = saved.settings;
  const { pass, appPass, ...mqtt } = s.mqtt;
  return { ...s, mqtt: { ...mqtt, hasPass: !!pass, hasAppPass: !!appPass } };
}

function run(text, via) {
  const v = verify(text);
  const reply = { type: 'reply' };
  if (v.re) reply.re = v.re;
  if (v.error) {
    if (['unknown_key', 'bad_signature', 'wrong_device'].includes(v.error)) post({ e: 'denied', why: v.error, via });
    return { reply: { ...reply, ok: false, error: v.error } };
  }
  const { p, key } = v;
  const done = (error, extra = {}) => ({ key, reply: { ...reply, ...extra, ok: !error, ...(error ? { error } : {}) } });
  switch (p.a) {
    case 'hello':
    case 'state':
      return done(null, { name: key.name, admin: key.admin, state: state() });
    case 'open':
      if (args.includes('--no-pins')) return done('no_pins');
      return done(open(key.name, via) ? null : 'busy');
    case 'auto':
      if (typeof p.min !== 'number' || p.min < 0 || p.min > 720) return done('invalid');
      live.autoUntil = p.min ? Date.now() + p.min * 60000 : 0;
      post({ e: 'auto', by: key.name, min: p.min });
      return done(null);
    case 'mute':
      if (typeof p.min !== 'number' || p.min < -1 || p.min > 1440) return done('invalid');
      live.mute = p.min < 0 ? -1 : p.min ? Date.now() + p.min * 60000 : 0;
      post({ e: 'mute', by: key.name, min: p.min });
      return done(null);
  }
  if (!['settings', 'keys', 'key_add', 'key_remove', 'reboot'].includes(p.a)) return done('unknown_action');
  if (!key.admin) return done('forbidden');
  if (via !== 'local') return done('local_only');
  switch (p.a) {
    case 'settings': {
      const error = p.set ? applySettings(p.set) : null;
      return done(error, error ? {} : { settings: settingsView() });
    }
    case 'keys':
      return done(null, { keys: saved.keys.map(({ id, name, admin, last }) => ({ id, name, admin, last })) });
    case 'key_add': {
      if (typeof p.name !== 'string' || !p.name || Buffer.byteLength(p.name) > 23) return done('invalid');
      const k = addKey(p.name, !!p.admin);
      if (!k) return done('full');
      const pair = pairing(k);
      const extra = { pair, localLink: link(`http://localhost:${port}/`, pair) };
      if (saved.settings.appUrl) extra.link = link(saved.settings.appUrl, pair);
      return done(null, extra);
    }
    case 'key_remove': {
      const k = saved.keys.find((x) => x.id === p.id);
      if (!k) return done('not_found');
      if (k.admin && saved.keys.filter((x) => x.admin).length === 1) return done('last_admin');
      saved.keys = saved.keys.filter((x) => x !== k);
      save();
      post({ e: 'key_remove', name: k.name });
      for (const c of clients) if (c.keyId === k.id) c.socket.destroy();
      return done(null);
    }
    case 'reboot':
      log('(reboot requested; the simulator keeps running)');
      return done(null);
  }
}

// Rings and the lobby code ------------------------------------------------------

function ring(line, ms) {
  log(`🔔 ${line} rings for ${ms} ms${live.mute ? ' (monitor muted)' : ''}`);
  live.ringing[line] = true;
  if (line === 'lobby') {
    live.code.pressing = true;
    clearTimeout(live.code.timer);
  }
  changed();
  setTimeout(() => {
    live.ringing[line] = false;
    post({ e: 'ring', line, ms });
    if (line !== 'lobby') return;
    const c = live.code;
    c.pressing = false;
    const { short, long } = saved.settings.code;
    c.seq += ms < short ? '.' : ms >= long ? '-' : '?'; // in between spoils the code, like the firmware
    c.timer = setTimeout(endCode, saved.settings.code.gap);
    if (live.autoUntil > Date.now()) setTimeout(() => open('auto-open', 'auto'), 500);
  }, ms);
}

function endCode() {
  const seq = live.code.seq;
  live.code.seq = '';
  const { on, seq: code } = saved.settings.code;
  if (!on) return;
  if (seq === code) {
    live.fails = 0;
    open('code', 'panel');
  } else if (seq.length >= Math.min(3, code.length)) {
    post({ e: 'code_fail', count: ++live.fails });
  }
}

async function typeCode() {
  for (const sym of saved.settings.code.seq) {
    ring('lobby', sym === '-' ? saved.settings.code.long + 300 : 250);
    await new Promise((r) => setTimeout(r, (sym === '-' ? saved.settings.code.long + 300 : 250) + 300));
  }
}

function holdButton() {
  live.claimUntil = Date.now() + 10 * 60 * 1000;
  log('🔘 button held: a phone can pair as admin for 10 min');
}

// Pushing state ---------------------------------------------------------------

let pushTimer = null;
function changed() {
  clearTimeout(pushTimer);
  pushTimer = setTimeout(() => {
    const text = JSON.stringify(state());
    for (const c of clients) if (c.keyId && saved.keys.some((k) => k.id === c.keyId)) c.socket.write(frame(Buffer.from(text)));
    if (remote?.connected) remote.publish(`${base}/state`, text, { retain: true });
  }, 50);
}
setInterval(() => {
  if (live.mute > 0 && live.mute <= Date.now()) {
    live.mute = 0;
    changed();
  }
}, 1000);

// HTTP and the WebSocket ---------------------------------------------------------

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };

async function body(req) {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  return Buffer.concat(chunks).toString('utf8');
}

function json(res, status, data) {
  res.writeHead(status, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(data));
}

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const path = url.pathname;
  if (path === '/api/info') {
    const setup = args.includes('--setup'); // pretend the phone is on the setup hotspot
    return json(res, 200, { v: VERSION, id: ID, name: saved.settings.name, fw: '0.1.0-sim', host: `localhost:${port}`, setup, viaSetup: setup, claim: live.claimUntil > Date.now(), paired: saved.keys.length > 0 });
  }
  if (path === '/api/wifi') return json(res, 200, { setup: false, configured: true, connected: true, ssid: 'Simulated Wi-Fi', ip: '127.0.0.1', host: `localhost:${port}`, setupSsid: 'Dumbledoor Setup', error: 0 });
  if (path === '/api/scan') return json(res, 200, [{ ssid: 'Simulated Wi-Fi', rssi: -50, open: false }, { ssid: 'Neighbour', rssi: -80, open: false }]);
  if (path === '/api/claim' && req.method === 'POST') {
    if (live.claimUntil <= Date.now()) return json(res, 403, { ok: false, error: 'claim_closed' });
    let name = 'Phone';
    try {
      name = JSON.parse(await body(req)).name || name;
    } catch {}
    live.claimUntil = 0;
    const key = addKey(name.slice(0, 23), true);
    return json(res, 200, { ok: true, pair: pairing(key) });
  }
  if (path === '/api/cmd' && req.method === 'POST') {
    const { reply } = run(await body(req), 'local');
    return json(res, reply.ok ? 200 : 400, reply);
  }
  if (path === '/api/ota' && req.method === 'POST') {
    const v = verify(req.headers['x-auth'] || '');
    const size = (await body(req)).length;
    if (v.error) return json(res, 401, { ok: false, error: v.error });
    if (v.p.a !== 'ota') return json(res, 400, { ok: false, error: 'unknown_action' });
    if (!v.key.admin) return json(res, 403, { ok: false, error: 'forbidden' });
    log(`(firmware upload of ${size} bytes accepted and ignored)`);
    return json(res, 200, { ok: true });
  }
  if (path === '/sim/ring' && req.method === 'POST') {
    ring(url.searchParams.get('line') || 'lobby', Number(url.searchParams.get('ms') || 300));
    return json(res, 200, { ok: true });
  }
  if (path === '/sim/code' && req.method === 'POST') {
    typeCode();
    return json(res, 200, { ok: true });
  }
  if (path === '/sim/button' && req.method === 'POST') {
    holdButton();
    return json(res, 200, { ok: true });
  }
  // The app's files.
  const file = normalize(join(appDir, path === '/' ? 'index.html' : path));
  if (!file.startsWith(appDir)) return json(res, 404, { ok: false });
  try {
    const data = await readFile(file);
    res.writeHead(200, { 'Content-Type': TYPES[extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
    res.end(data);
  } catch {
    json(res, 404, { ok: false, error: 'not_found' });
  }
});

const clients = new Set();

function frame(data, opcode = 1) {
  const n = data.length;
  let head;
  if (n < 126) head = Buffer.from([0x80 | opcode, n]);
  else if (n < 65536) head = Buffer.from([0x80 | opcode, 126, n >> 8, n & 255]);
  else {
    head = Buffer.alloc(10);
    head[0] = 0x80 | opcode;
    head[1] = 127;
    head.writeBigUInt64BE(BigInt(n), 2);
  }
  return Buffer.concat([head, data]);
}

function readFrames(client) {
  for (;;) {
    const b = client.buf;
    if (b.length < 2) return;
    const opcode = b[0] & 0x0f;
    const masked = b[1] & 0x80;
    let len = b[1] & 0x7f;
    let off = 2;
    if (len === 126) {
      if (b.length < 4) return;
      len = b.readUInt16BE(2);
      off = 4;
    } else if (len === 127) {
      if (b.length < 10) return;
      len = Number(b.readBigUInt64BE(2));
      off = 10;
    }
    const maskLen = masked ? 4 : 0;
    if (b.length < off + maskLen + len) return;
    let payload = Buffer.from(b.subarray(off + maskLen, off + maskLen + len));
    if (masked) {
      const mask = b.subarray(off, off + 4);
      for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
    }
    client.buf = b.subarray(off + maskLen + len);
    if (opcode === 1) {
      const { key, reply } = run(payload.toString('utf8'), 'local');
      if (key) client.keyId = key.id;
      client.socket.write(frame(Buffer.from(JSON.stringify(reply))));
    } else if (opcode === 8) {
      client.socket.end(frame(Buffer.alloc(0), 8));
    } else if (opcode === 9) {
      client.socket.write(frame(payload, 10));
    }
  }
}

server.on('upgrade', (req, socket) => {
  if (req.url !== '/ws') return socket.destroy();
  const accept = createHash('sha1').update(`${req.headers['sec-websocket-key']}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest('base64');
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  const client = { socket, buf: Buffer.alloc(0), keyId: null };
  clients.add(client);
  socket.on('data', (chunk) => {
    client.buf = Buffer.concat([client.buf, chunk]);
    readFrames(client);
  });
  socket.on('close', () => clients.delete(client));
  socket.on('error', () => clients.delete(client));
});

// MQTT: the remote path ------------------------------------------------------------

let remote = null;
const base = `${saved.settings.mqtt.prefix || 'dumbledoor'}/${ID}`;

async function connectRemote() {
  if (!mqttUrl) return;
  remote = new MqttClient(mqttUrl, { clientId: ID, username: opt('user', ''), password: opt('pass', '') });
  remote.onmessage = (topic, text) => {
    if (topic !== `${base}/cmd`) return;
    const { reply } = run(text, 'remote');
    remote.publish(`${base}/reply`, JSON.stringify(reply));
  };
  remote.onclose = () => {
    log('lost the broker, reconnecting in 5 s');
    setTimeout(connectRemote, 5000);
  };
  try {
    await remote.connect();
    await remote.subscribe([`${base}/cmd`]);
    remote.publish(`${base}/status`, 'online', { retain: true });
    log(`connected to ${mqttUrl} as ${base}`);
    changed();
  } catch (err) {
    log(`can't reach the broker (${err.message}), retrying in 5 s`);
    setTimeout(connectRemote, 5000);
  }
}

function quit() {
  if (remote?.connected) remote.publish(`${base}/status`, 'offline', { retain: true });
  setTimeout(() => process.exit(0), 300);
}
process.on('SIGINT', quit);
process.on('SIGTERM', quit);

server.listen(port, () => {
  log(`Dumbledoor simulator on http://localhost:${port}  (${saved.keys.length} key(s)${live.claimUntil ? ', a phone can pair now' : ''})`);
  post({ e: 'boot', reason: 'power-on' });
  connectRemote();
  if (process.stdin.isTTY) {
    log('keys: r short ring · l long ring · f front door · c code · b button · q quit');
    process.stdin.setRawMode(true);
    process.stdin.on('data', (k) => {
      const key = k.toString();
      if (key === 'r') ring('lobby', 300);
      else if (key === 'l') ring('lobby', 1200);
      else if (key === 'f') ring('front', 600);
      else if (key === 'c') typeCode();
      else if (key === 'b') holdButton();
      else if (key === 'q' || key === '\u0003') quit();
    });
  }
});
