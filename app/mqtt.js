// A small MQTT 3.1.1 client over WebSockets: connect, subscribe and publish at QoS 0,
// which is all the app needs to reach Dumbledoor through a broker. No dependencies,
// so the device can serve it too. Works in browsers and in Node 22+ (tools/sim).

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function concat(parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let offset = 0;
  for (const p of parts) {
    out.set(p, offset);
    offset += p.length;
  }
  return out;
}

const u16 = (n) => Uint8Array.of(n >> 8, n & 0xff);

function string(text) {
  const bytes = encoder.encode(text);
  return concat([u16(bytes.length), bytes]);
}

function packet(header, ...body) {
  const payload = concat(body);
  const length = [];
  let n = payload.length;
  do {
    let digit = n % 128;
    n = Math.floor(n / 128);
    if (n > 0) digit |= 0x80;
    length.push(digit);
  } while (n > 0);
  return concat([Uint8Array.of(header, ...length), payload]);
}

const CONNACK_ERRORS = {
  1: 'the broker doesn’t speak MQTT 3.1.1',
  2: 'client ID rejected',
  3: 'broker unavailable',
  4: 'wrong user name or password',
  5: 'not authorized',
};

export class MqttClient {
  // url: wss://host:port/path. options: { username, password, clientId, keepalive (s) }
  constructor(url, options = {}) {
    this.url = url;
    this.options = { keepalive: 30, clientId: '', ...options };
    this.onmessage = null; // (topic, text, retained) => void
    this.onclose = null; // () => void, after a connection that had succeeded
    this.ws = null;
    this.buffer = new Uint8Array();
    this.nextId = 1;
    this.waiting = new Map(); // packet id → resolve, for SUBACKs
    this.timer = null;
    this.lastHeard = 0;
  }

  connect() {
    return new Promise((resolve, reject) => {
      let settled = false;
      const ws = new WebSocket(this.url, ['mqtt']);
      ws.binaryType = 'arraybuffer';
      this.ws = ws;
      const fail = (message) => {
        if (!settled) {
          settled = true;
          clearTimeout(timeout);
          reject(new Error(message));
        }
        ws.close();
      };
      const timeout = setTimeout(() => fail('timed out'), 10000);
      this.connack = (code) => {
        if (code !== 0) return fail(CONNACK_ERRORS[code] || `refused (${code})`);
        settled = true;
        clearTimeout(timeout);
        this.startKeepalive();
        resolve();
      };
      ws.onopen = () => this.send(this.connectPacket());
      ws.onmessage = (event) => this.feed(new Uint8Array(event.data));
      ws.onerror = () => fail('can’t reach the broker');
      ws.onclose = () => {
        clearInterval(this.timer);
        for (const resolveSub of this.waiting.values()) resolveSub(false);
        this.waiting.clear();
        if (settled) this.onclose?.();
        else fail('connection closed');
      };
    });
  }

  connectPacket() {
    const { username, password, clientId, keepalive } = this.options;
    const flags = 0x02 | (username ? 0x80 : 0) | (username && password ? 0x40 : 0); // clean session
    return packet(
      0x10,
      string('MQTT'),
      Uint8Array.of(4, flags),
      u16(keepalive),
      string(clientId),
      username ? string(username) : new Uint8Array(),
      username && password ? string(password) : new Uint8Array(),
    );
  }

  // Resolves to true once the broker accepted every topic.
  subscribe(topics) {
    const id = this.nextId;
    this.nextId = (this.nextId % 0xffff) + 1; // packet ids run 1..65535
    const filters = topics.map((t) => concat([string(t), Uint8Array.of(0)]));
    return new Promise((resolve) => {
      this.waiting.set(id, resolve);
      this.send(packet(0x82, u16(id), ...filters));
    });
  }

  publish(topic, text, { retain = false } = {}) {
    this.send(packet(0x30 | (retain ? 1 : 0), string(topic), encoder.encode(text)));
  }

  close() {
    if (this.ws?.readyState === WebSocket.OPEN) this.send(Uint8Array.of(0xe0, 0));
    this.onclose = null;
    this.ws?.close();
  }

  get connected() {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  send(bytes) {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(bytes);
  }

  // Pings at 3/4 of the keepalive, and gives up on a broker that went quiet.
  startKeepalive() {
    const ms = this.options.keepalive * 1000;
    this.lastHeard = Date.now();
    this.timer = setInterval(() => {
      if (Date.now() - this.lastHeard > ms * 1.5) this.ws.close();
      else this.send(Uint8Array.of(0xc0, 0));
    }, ms * 0.75);
  }

  // WebSocket messages may hold several MQTT packets, or part of one.
  feed(chunk) {
    this.lastHeard = Date.now();
    this.buffer = this.buffer.length ? concat([this.buffer, chunk]) : chunk;
    for (;;) {
      let length = 0;
      let multiplier = 1;
      let i = 1;
      let digit;
      do {
        if (i >= this.buffer.length) return;
        digit = this.buffer[i++];
        length += (digit & 0x7f) * multiplier;
        multiplier *= 128;
      } while (digit & 0x80 && i < 5);
      if (this.buffer.length < i + length) return;
      const header = this.buffer[0];
      const body = this.buffer.subarray(i, i + length);
      this.buffer = this.buffer.slice(i + length);
      this.handle(header, body);
    }
  }

  handle(header, body) {
    switch (header >> 4) {
      case 2: // CONNACK
        this.connack?.(body[1]);
        break;
      case 3: {
        // PUBLISH
        const qos = (header >> 1) & 3;
        const topicLength = (body[0] << 8) | body[1];
        const topic = decoder.decode(body.subarray(2, 2 + topicLength));
        let offset = 2 + topicLength;
        if (qos > 0) {
          const id = body.subarray(offset, offset + 2);
          offset += 2;
          this.send(concat([Uint8Array.of(qos === 1 ? 0x40 : 0x50, 2), id])); // PUBACK, or PUBREC
        }
        this.onmessage?.(topic, decoder.decode(body.subarray(offset)), (header & 1) === 1);
        break;
      }
      case 6: // PUBREL (QoS 2): complete it
        this.send(concat([Uint8Array.of(0x70, 2), body.subarray(0, 2)]));
        break;
      case 9: {
        // SUBACK
        const id = (body[0] << 8) | body[1];
        const granted = Array.from(body.subarray(2));
        this.waiting.get(id)?.(granted.every((code) => code !== 0x80));
        this.waiting.delete(id);
        break;
      }
      default: // PINGRESP and the rest only matter as signs of life
        break;
    }
  }
}
