// Talking to Dumbledoor: on the home network over its WebSocket, or from anywhere
// through the MQTT broker. Either way the app sends signed commands and receives the
// same state object. See docs/protocol.md.

import { fromHex, hmacSha256, randomHex, toHex, utf8 } from './crypto.js';
import { MqttClient } from './mqtt.js';

// localStorage can be missing or throw (private windows, blocked storage).
const memory = new Map();
export const storage = {
  get(key) {
    try {
      return localStorage.getItem(key);
    } catch {
      return memory.get(key) ?? null;
    }
  },
  set(key, value) {
    try {
      localStorage.setItem(key, value);
    } catch {
      memory.set(key, value);
    }
  },
  remove(key) {
    try {
      localStorage.removeItem(key);
    } catch {
      memory.delete(key);
    }
  },
};

// Every command carries a timestamp newer than the last one this key sent, so a
// recorded command can't be played back later. The device also checks it against its
// own clock (±2 minutes) once that has been set over NTP.
function nextTimestamp(keyId) {
  const name = `dumbledoor.t.${keyId}`;
  const t = Math.max(Date.now(), Number(storage.get(name) || 0) + 1);
  storage.set(name, String(t));
  return t;
}

// `args` can also be a function of the timestamp, for fields sealed with it.
export function sign(pairing, action, args = {}) {
  const t = nextTimestamp(pairing.k);
  const p = JSON.stringify({ d: pairing.d, t, a: action, ...(typeof args === 'function' ? args(t) : args) });
  const s = toHex(hmacSha256(fromHex(pairing.s), utf8(p)));
  return { text: JSON.stringify({ k: pairing.k, p, s }), re: s.slice(0, 16), t };
}

// The lobby code's presses travel sealed, so the broker and the other phones on it can't
// read them: zero-padded to 12 bytes and XORed with HMAC-SHA256(this phone's secret,
// "code>" + t) on the way to Dumbledoor, or "code<" + t on the way back, where t is the
// command's timestamp. See docs/protocol.md.
const CODE_MAX = 12;
const codeKeystream = (pairing, dir, t) => hmacSha256(fromHex(pairing.s), utf8(`code${dir}${t}`));

export function sealCode(pairing, dir, t, seq) {
  const ks = codeKeystream(pairing, dir, t);
  const bytes = new Uint8Array(CODE_MAX);
  bytes.set(utf8(seq).subarray(0, CODE_MAX));
  return toHex(bytes.map((b, i) => b ^ ks[i]));
}

// The presses, or null if `sealed` doesn't open to any.
export function unsealCode(pairing, dir, t, sealed) {
  if (!/^[0-9a-f]{24}$/.test(sealed)) return null;
  const ks = codeKeystream(pairing, dir, t);
  const seq = String.fromCharCode(...fromHex(sealed).map((b, i) => b ^ ks[i])).replace(/\0+$/, '');
  return /^[.-]*$/.test(seq) ? seq : null;
}

// Through the broker, an admin's full state comes sealed with its key as
// {n: nonce, m: tag, x: base64}: K = HMAC(secret, "state<" + n), block i of the keystream
// is HMAC(K, "k" + i), and m is the first 16 bytes of HMAC(K, "m" + ciphertext). Returns
// the state, or null if the message was changed or isn't for this key.
export function unsealState(pairing, text) {
  try {
    const { n, m, x } = JSON.parse(text);
    if (!/^[0-9a-f]{16}$/.test(n) || !/^[0-9a-f]{32}$/.test(m)) return null;
    const k = hmacSha256(fromHex(pairing.s), utf8(`state<${n}`));
    const tagged = Uint8Array.from(`m${atob(x)}`, (c) => c.charCodeAt(0));
    if (toHex(hmacSha256(k, tagged).subarray(0, 16)) !== m) return null;
    const bytes = tagged.subarray(1);
    for (let off = 0; off < bytes.length; off += 32) {
      const ks = hmacSha256(k, utf8(`k${off / 32}`));
      for (let i = 0; i < 32 && off + i < bytes.length; i++) bytes[off + i] ^= ks[i];
    }
    return JSON.parse(new TextDecoder().decode(bytes));
  } catch {
    return null;
  }
}

const backoff = (attempt) => Math.min(15000, 1000 * 2 ** attempt);

// The WebSocket the device serves. It pushes state once a signed "hello" checks out.
class LocalLink {
  constructor(device) {
    this.device = device;
    this.ws = null;
    this.ready = false;
  }

  open() {
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(`${scheme}://${location.host}/ws`);
    this.ws = ws;
    ws.onopen = () => {
      const { text, re } = sign(this.device.pairing, 'hello');
      this.device.expect(re, (reply) => {
        if (reply.ok) {
          this.ready = true;
          this.device.setStatus('online');
          if (reply.state) this.device.setState(reply.state);
        } else {
          this.device.setStatus(reply.error === 'unknown_key' ? 'denied' : 'error', reply.error);
          ws.close();
        }
      });
      ws.send(text);
    };
    ws.onmessage = (event) => {
      const msg = JSON.parse(event.data);
      if (msg.type === 'state') this.device.setState(msg);
      else if (msg.type === 'reply') this.device.resolve(msg);
    };
    ws.onclose = () => {
      this.ready = false;
      this.device.linkClosed(this);
    };
  }

  send(text) {
    this.ws.send(text);
  }

  close() {
    this.ws.onclose = null;
    this.ws.close();
  }
}

// The broker: the device publishes its state there and listens for commands. Every phone
// with the broker login can read the plain state topic, so it only has the short state;
// admins get the full one on their own topic, sealed with their key.
class RemoteLink {
  constructor(device) {
    this.device = device;
    this.ready = false;
    const m = device.pairing.m;
    this.base = `${m.t || 'dumbledoor'}/${device.pairing.d}`;
    this.own = device.pairing.a ? `${this.base}/state/${device.pairing.k}` : '';
    this.sealed = false; // an admin has had its full state
    this.client = new MqttClient(m.u, { username: m.user, password: m.pass, clientId: `dd-app-${randomHex(4)}` });
  }

  async open() {
    const client = this.client;
    client.onmessage = (topic, text) => {
      const kind = topic.slice(this.base.length + 1);
      if (topic === this.own) {
        const state = text && unsealState(this.device.pairing, text);
        if (state) {
          this.sealed = true;
          this.device.setState(state);
        }
      } else if (kind === 'status') {
        this.ready = text === 'online';
        this.device.setStatus(this.ready ? 'online' : 'offline');
      } else if (kind === 'state' && text) {
        // Admins only take the plain state from firmware that put everything there.
        const state = JSON.parse(text);
        if (!this.own || (!this.sealed && state.events)) this.device.setState(state);
      } else if (kind === 'reply') {
        this.device.resolve(JSON.parse(text));
      }
    };
    client.onclose = () => {
      this.ready = false;
      this.device.linkClosed(this);
    };
    try {
      await client.connect();
      const topics = [`${this.base}/status`, `${this.base}/state`, `${this.base}/reply`];
      await client.subscribe(this.own ? [...topics, this.own] : topics);
      // The status is retained, so it arrives right away unless the device never connected.
      setTimeout(() => {
        if (this.device.link === this && this.device.status === 'connecting') {
          this.device.setStatus('offline', 'never_connected');
        }
      }, 4000);
    } catch (err) {
      this.device.setStatus('offline', err.message);
      this.device.linkClosed(this);
    }
  }

  send(text) {
    this.client.publish(`${this.base}/cmd`, text);
  }

  close() {
    this.client.close();
  }
}

// Events: "status" (this.status: connecting, online, offline, denied, error) and "state".
export class Dumbledoor extends EventTarget {
  constructor(pairing, mode) {
    super();
    this.pairing = pairing;
    this.mode = mode; // "local" or "remote"
    this.status = 'connecting';
    this.detail = '';
    this.state = null;
    this.stateAt = 0; // when this.state arrived (Date.now())
    this.link = null;
    this.pending = new Map(); // reply "re" → callback
    this.attempt = 0;
    this.retry = null;
    this.stopped = true;
  }

  start() {
    this.stopped = false;
    this.connect();
  }

  stop() {
    this.stopped = true;
    clearTimeout(this.retry);
    this.link?.close();
    this.link = null;
  }

  connect() {
    clearTimeout(this.retry);
    this.link?.close();
    this.setStatus('connecting');
    this.link = this.mode === 'local' ? new LocalLink(this) : new RemoteLink(this);
    this.link.open();
  }

  // Reconnects now instead of waiting out the backoff, e.g. when the page comes back.
  // `force` also replaces a link that looks fine: after a long sleep it may be dead.
  wake(force = false) {
    if (!this.stopped && this.status !== 'denied' && (force || !this.link?.ready)) {
      this.attempt = 0;
      this.connect();
    }
  }

  linkClosed(link) {
    if (link !== this.link) return;
    this.link = null;
    for (const [re, done] of this.pending) {
      done({ ok: false, error: 'offline', re });
    }
    this.pending.clear();
    // A sign-in error stays on screen while it keeps retrying; a removed key stops it.
    if (this.status === 'online' || this.status === 'connecting') this.setStatus('offline', this.detail);
    if (this.stopped || this.status === 'denied') return;
    this.retry = setTimeout(() => this.connect(), backoff(this.attempt++));
  }

  setStatus(status, detail = '') {
    if (status === 'online') this.attempt = 0;
    this.status = status;
    this.detail = detail;
    this.dispatchEvent(new Event('status'));
  }

  setState(state) {
    this.state = state;
    this.stateAt = Date.now();
    this.dispatchEvent(new Event('state'));
  }

  expect(re, callback) {
    this.pending.set(re, callback);
  }

  resolve(reply) {
    const done = this.pending.get(reply.re);
    if (!done) return; // someone else's reply on the shared broker topic
    this.pending.delete(reply.re);
    done(reply);
  }

  get ready() {
    return !!this.link?.ready;
  }

  // The lobby code (admins): reads it, or changes the fields in `set`. Resolves to the
  // reply with code.seq opened.
  async code(set) {
    let t = 0;
    const reply = await this.send('code', (time) => {
      t = time;
      if (!set) return {};
      return { set: 'seq' in set ? { ...set, seq: sealCode(this.pairing, '>', time, set.seq) } : set };
    });
    if (!reply.ok) return reply;
    const seq = unsealCode(this.pairing, '<', t, reply.code?.seq);
    return seq === null ? { ...reply, ok: false, error: 'garbled' } : { ...reply, code: { ...reply.code, seq } };
  }

  // Sends a signed command and resolves to the reply ({ ok, error, ... }). Never rejects.
  send(action, args = {}, timeout = 8000) {
    if (!this.ready) return Promise.resolve({ ok: false, error: 'offline' });
    const { text, re } = sign(this.pairing, action, args);
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(re);
        resolve({ ok: false, error: 'timeout' });
      }, timeout);
      this.expect(re, (reply) => {
        clearTimeout(timer);
        resolve(reply);
      });
      this.link.send(text);
    });
  }
}
