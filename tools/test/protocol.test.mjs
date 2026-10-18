// Signing, replay protection and permissions, against the simulator (which mirrors the
// firmware's checks). Commands are signed with the app's own code.
import { createHmac } from 'node:crypto';
import { sign } from '../../app/device.js';
import { check, done, startSim } from './util.mjs';

const sim = await startSim();
const cmd = (envelope) => sim.post('/api/cmd', envelope);
const run = (pairing, action, args) => cmd(sign(pairing, action, args).text);

try {
  const claim = await sim.post('/api/claim', JSON.stringify({ name: 'Admin' }));
  check('the first phone becomes admin', claim.ok && claim.pair.a === true);
  const admin = claim.pair;
  const again = await sim.post('/api/claim', JSON.stringify({ name: 'Sneaky' }));
  check('nobody else can claim afterwards', !again.ok && again.error === 'claim_closed');

  const state = await run(admin, 'state');
  check('a signed command works', state.ok && state.state.id === admin.d);

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
  check('the new phone works', (await run(guest, 'state')).ok);
  check('a non-admin can’t add phones', (await run(guest, 'key_add', { name: 'x' })).error === 'forbidden');
  check('a non-admin can’t change settings', (await run(guest, 'settings', { set: { name: 'x' } })).error === 'forbidden');
  check('a non-admin can use auto-open', (await run(guest, 'auto', { min: 15 })).ok);
  check('out-of-range values are refused', (await run(guest, 'mute', { min: 99999 })).error === 'invalid');

  const keys = await run(admin, 'keys');
  check('two keys listed', keys.ok && keys.keys.length === 2);
  check('the only admin can’t be removed', (await run(admin, 'key_remove', { id: admin.k })).error === 'last_admin');
  check('removing the guest', (await run(admin, 'key_remove', { id: guest.k })).ok);
  check('the removed phone is locked out', (await run(guest, 'state')).error === 'unknown_key');

  const settings = await run(admin, 'settings', { set: { mqtt: { pass: 'secret' } } });
  check('passwords are never sent back', settings.ok && !JSON.stringify(settings).includes('secret') && settings.settings.mqtt.hasPass);
} finally {
  await sim.stop();
}
done('protocol');
