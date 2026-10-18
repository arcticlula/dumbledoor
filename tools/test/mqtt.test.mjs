// app/mqtt.js against public test brokers (needs internet). Uses a random topic.
import { randomBytes } from 'node:crypto';
import { MqttClient } from '../../app/mqtt.js';
import { check, done, sleep } from './util.mjs';

const brokers = process.argv.slice(2).length
  ? process.argv.slice(2)
  : ['wss://broker.hivemq.com:8884/mqtt', 'wss://test.mosquitto.org:8081/'];

for (const url of brokers) {
  const base = `dumbledoor-test/${randomBytes(6).toString('hex')}`;
  const a = new MqttClient(url, { clientId: `dd-test-${randomBytes(3).toString('hex')}` });
  const b = new MqttClient(url, { clientId: `dd-test-${randomBytes(3).toString('hex')}` });
  try {
    await a.connect();
    a.publish(`${base}/state`, JSON.stringify({ text: 'ré ✨', big: 'x'.repeat(3000) }), { retain: true });
    await sleep(800);
    await b.connect();
    const got = [];
    b.onmessage = (topic, text, retained) => got.push({ topic, text, retained });
    const subscribed = await b.subscribe([`${base}/state`, `${base}/cmd`]);
    a.publish(`${base}/cmd`, 'open please');
    await sleep(1500);
    const state = got.find((m) => m.topic.endsWith('/state'));
    const live = got.find((m) => m.topic.endsWith('/cmd'));
    check(`${url}: subscribed`, subscribed);
    check(`${url}: retained message, 3 KB, UTF-8`, !!state && state.retained && JSON.parse(state.text).text === 'ré ✨');
    check(`${url}: live message`, !!live && !live.retained && live.text === 'open please');
    a.publish(`${base}/state`, '', { retain: true }); // clear the retained message
    await sleep(300);
  } catch (err) {
    check(`${url}: connect`, false, err.message);
  } finally {
    a.close();
    b.close();
  }
}
done('mqtt');
