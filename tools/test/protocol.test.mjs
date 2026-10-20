// Signing, replay protection and permissions, against the simulator (which mirrors the
// firmware's checks). Commands are signed with the app's own code.
import { createHmac } from 'node:crypto';
import { sealCode, sign, unsealCode } from '../../app/device.js';
import { check, done, sleep, startSim } from './util.mjs';

const sim = await startSim();
const cmd = (envelope) => sim.post('/api/cmd', envelope);
const run = (pairing, action, args) => cmd(sign(pairing, action, args).text);
const viaBroker = (pairing, action, args) => sim.post('/sim/remote', sign(pairing, action, args).text);

// The lobby code, sealed as the app does it. `path` /sim/remote runs it as if it came
// through the broker.
async function code(pairing, set, path = '/api/cmd') {
  const seal = (t) => ('seq' in set ? { ...set, seq: sealCode(pairing, '>', t, set.seq) } : set);
  const signed = sign(pairing, 'code', (t) => (set ? { set: seal(t) } : {}));
  const reply = await sim.post(path, signed.text);
  const sent = JSON.parse(JSON.parse(signed.text).p).set?.seq;
  return { ...reply, sent, seq: reply.ok ? unsealCode(pairing, '<', signed.t, reply.code.seq) : null };
}

try {
  const claim = await sim.post('/api/claim', JSON.stringify({ name: 'Admin' }));
  check('the first phone becomes admin', claim.ok && claim.pair.a === true);
  const admin = claim.pair;
  const again = await sim.post('/api/claim', JSON.stringify({ name: 'Sneaky' }));
  check('nobody else can claim afterwards', !again.ok && again.error === 'claim_closed');

  const state = await run(admin, 'state');
  check('a signed command works', state.ok && state.state.id === admin.d);
  check('an admin at home gets the full state', Array.isArray(state.state.events) && 'ringing' in state.state);

  const once = sign(admin, 'state').text;
  check('first send of a command', (await cmd(once)).ok);
  check('the same command again is refused', (await cmd(once)).error === 'replay');

  const signWith = (pairing, payload) => {
    const p = JSON.stringify(payload);
    const s = createHmac('sha256', Buffer.from(pairing.s, 'hex')).update(p).digest('hex');
    return JSON.stringify({ k: pairing.k, p, s });
  };
  const future = Date.now() + 10 * 60 * 1000;
  check('a timestamp 10 min off is refused', (await cmd(signWith(admin, { d: admin.d, t: future, a: 'state' }))).error === 'clock');
  check('a key for another device is refused', (await cmd(signWith(admin, { d: 'dd-000000', t: Date.now() + 1000, a: 'open' }))).error === 'wrong_device');

  const tampered = JSON.parse(sign(admin, 'state').text);
  tampered.p = tampered.p.replace('"state"', '"open"');
  check('an altered command is refused', (await cmd(JSON.stringify(tampered))).error === 'bad_signature');
  const stranger = JSON.parse(sign(admin, 'open').text);
  stranger.k = 'deadbeef';
  check('an unknown key is refused', (await cmd(JSON.stringify(stranger))).error === 'unknown_key');
  check('junk is refused', (await cmd('{"hello":')).error === 'bad_request');

  check('open', (await run(admin, 'open')).ok);
  check('open again during the cooldown', (await run(admin, 'open')).error === 'busy');

  const added = await run(admin, 'key_add', { name: 'Guest', admin: false });
  check('admin adds a phone, with a link', added.ok && added.localLink.includes('#pair='));
  const guest = added.pair;
  const guestState = await run(guest, 'state');
  check('the new phone works', guestState.ok && guestState.admin === false);
  check(
    'it only gets what opening needs',
    Object.keys(guestState.state).sort().join() === 'id,name,pins,type,v',
    Object.keys(guestState.state).join(),
  );
  check('a non-admin can’t add phones', (await run(guest, 'key_add', { name: 'x' })).error === 'forbidden');
  check('a non-admin can’t change settings', (await run(guest, 'settings', { set: { name: 'x' } })).error === 'forbidden');
  check('a non-admin can’t use auto-open', (await run(guest, 'auto', { min: 15 })).error === 'forbidden');
  check('…nor mute the monitor', (await run(guest, 'mute', { min: 60 })).error === 'forbidden');
  const guestOpen = await run(guest, 'open');
  check('…but can open', guestOpen.ok || guestOpen.error === 'busy'); // busy: the admin just opened it
  check('a non-admin can’t see the lobby code', (await run(guest, 'code')).error === 'forbidden');
  check('…nor change it through the broker', (await code(guest, { seq: '-' }, '/sim/remote')).error === 'forbidden');
  check('out-of-range values are refused', (await run(admin, 'mute', { min: 99999 })).error === 'invalid');

  const keys = await run(admin, 'keys');
  check('two keys listed', keys.ok && keys.keys.length === 2);
  check('the only admin can’t be removed', (await run(admin, 'key_remove', { id: admin.k })).error === 'last_admin');
  check('removing the guest', (await run(admin, 'key_remove', { id: guest.k })).ok);
  check('the removed phone is locked out', (await run(guest, 'state')).error === 'unknown_key');

  const saved = await code(admin, { on: true, seq: '.-.', short: 300, long: 800, gap: 2000 });
  check('admin sets the lobby code', saved.ok && saved.seq === '.-.' && saved.code.on && saved.code.short === 300);
  check(
    'the presses travel sealed, both ways',
    /^[0-9a-f]{24}$/.test(saved.sent) && /^[0-9a-f]{24}$/.test(saved.code.seq) && saved.sent !== saved.code.seq,
  );
  const read = await code(admin);
  check('reading it back', read.ok && read.seq === '.-.' && read.code.gap === 2000 && read.code.seq !== saved.code.seq);
  check('a plain-text code is refused', (await run(admin, 'code', { set: { seq: '..-' } })).error === 'invalid_code');
  const wrongTime = sign(admin, 'code', (t) => ({ set: { seq: sealCode(admin, '>', t + 1, '..-') } }));
  check('a code sealed for another command is refused', (await cmd(wrongTime.text)).error === 'invalid_code');
  check('an empty code can’t be on', (await code(admin, { on: true, seq: '' })).error === 'invalid_code');
  const remote = await code(admin, { seq: '..--' }, '/sim/remote');
  check('admins change the code through the broker too', remote.ok && remote.seq === '..--' && remote.code.short === 300);
  check('settings don’t include the code', !('code' in (await run(admin, 'settings')).settings));
  check('settings stay home-only', (await viaBroker(admin, 'settings')).error === 'local_only');
  check('so do keys', (await viaBroker(admin, 'key_add', { name: 'Remote' })).error === 'local_only');
  // While the code is on, lobby presses only show up once they stop, as one outcome.
  await code(admin, { gap: 300 });
  const latest = async () => (await run(admin, 'state')).state;
  const since = async (n) => (await latest()).events.filter((e) => e.n > n);
  const press = async (...lengths) => {
    for (const ms of lengths) {
      await sim.post(`/sim/ring?line=lobby&ms=${ms}`);
      await sleep(ms + 150); // shorter than the 300 ms gap: still the same code
    }
    await sleep(400);
  };
  let last = (await latest()).events.at(-1).n;
  await sim.post('/sim/ring?line=lobby&ms=300');
  await sleep(100);
  const during = await latest();
  check('with the code on, a press doesn’t show while it lasts', during.ringing.lobby === false && during.events.at(-1).n === last);
  await sleep(700);
  let shown = await since(last);
  check('…but once the presses stop, as a ring', shown.length === 1 && shown[0].e === 'ring' && shown[0].ms === 300, JSON.stringify(shown));
  last = shown.at(-1)?.n ?? last;
  const ws = new WebSocket(`ws://localhost:${sim.port}/ws`); // an admin's app at home
  await new Promise((resolve) => (ws.onopen = resolve));
  const pushes = [];
  ws.onmessage = (m) => pushes.push(JSON.parse(m.data));
  ws.send(sign(admin, 'hello').text);
  await sleep(300);
  pushes.length = 0;
  await press(150, 150, 900);
  ws.close();
  shown = await since(last);
  check('a wrong code is one entry, with its pattern', shown.length === 1 && shown[0].e === 'code_fail' && shown[0].seq === '..-', JSON.stringify(shown));
  check('…and one state push, nothing while the presses last', pushes.filter((m) => m.type === 'state').length === 1, `${pushes.length} pushes`);
  last = shown.at(-1)?.n ?? last;
  await press(150, 150, 900, 900);
  shown = await since(last);
  check('the right code is just the opening', shown.length === 1 && shown[0].e === 'open' && shown[0].via === 'panel', JSON.stringify(shown));
  await run(admin, 'auto', { min: 15 });
  await sleep(3200); // the door's cooldown
  last = (await latest()).events.at(-1).n;
  await sim.post('/sim/ring?line=lobby&ms=150');
  await sleep(300);
  await sim.post('/sim/ring?line=lobby&ms=150');
  await sleep(80);
  const mid = await since(last);
  await sleep(700);
  shown = await since(last);
  check(
    'with the code on, auto-open also waits for the presses to stop',
    mid.length === 0 && shown.map((e) => e.by || e.e).join() === 'ring,ring,auto-open',
    JSON.stringify(shown),
  );
  await run(admin, 'auto', { min: 0 });

  const remoteState = await viaBroker(admin, 'state');
  check('state replies through the broker are short, even for admins', remoteState.ok && !('events' in remoteState.state));

  await run(admin, 'settings', { set: { appUrl: 'https://example.com/dumbledoor/' } });
  const homeOnly = await run(admin, 'key_add', { name: 'No broker' });
  check('no online-app link without broker details', homeOnly.ok && !homeOnly.link && homeOnly.localLink);

  const settings = await run(admin, 'settings', { set: { mqtt: { pass: 'secret' } } });
  check('passwords are never sent back', settings.ok && !JSON.stringify(settings).includes('secret') && settings.settings.mqtt.hasPass);
} finally {
  await sim.stop();
}
done('protocol');
