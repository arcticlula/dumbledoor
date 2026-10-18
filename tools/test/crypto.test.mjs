// app/crypto.js against Node's own SHA-256 and HMAC.
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { fromBase64Url, fromHex, hmacSha256, sha256, toHex, utf8 } from '../../app/crypto.js';
import { check, done } from './util.mjs';

check('sha256 of ""', toHex(sha256(new Uint8Array())) === 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
check('sha256 of "abc"', toHex(sha256(utf8('abc'))) === 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
check(
  'HMAC, RFC 4231 test case 2',
  toHex(hmacSha256(utf8('Jefe'), utf8('what do ya want for nothing?'))) === '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843',
);

let mismatches = 0;
for (let n = 0; n < 300; n++) {
  const msg = randomBytes(n);
  if (toHex(sha256(msg)) !== createHash('sha256').update(msg).digest('hex')) mismatches++;
  for (const keyLength of [0, 1, 32, 63, 64, 65, 100]) {
    const key = randomBytes(keyLength);
    if (toHex(hmacSha256(key, msg)) !== createHmac('sha256', key).update(msg).digest('hex')) mismatches++;
  }
}
check('2,400 random messages and keys', mismatches === 0, `${mismatches} mismatches`);
const big = randomBytes(200000);
check('a 200 KB message', toHex(sha256(big)) === createHash('sha256').update(big).digest('hex'));

check('hex round trip', toHex(fromHex('00ff10ab')) === '00ff10ab');
const json = JSON.stringify({ n: 'Luís — phone ✨', s: 'x'.repeat(10) });
check('base64url with non-ASCII text', fromBase64Url(Buffer.from(json).toString('base64url')) === json);

done('crypto');
