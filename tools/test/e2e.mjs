// The whole app in a headless phone-sized browser, against the simulator: pairing, rings,
// hold-to-open, auto-open, mute, the lobby code, adding and removing a phone.
// cd tools && npm install && npx playwright install chromium && npm run e2e
// SHOTS=/some/dir saves screenshots along the way.
import { chromium } from 'playwright';
import { check, done, sleep, startSim } from './util.mjs';

const sim = await startSim();
const SHOTS = process.env.SHOTS;
const browser = await chromium.launch();
const phone = { viewport: { width: 412, height: 915 }, deviceScaleFactor: 2, isMobile: true };
const errors = [];

async function newPhone(url) {
  const page = await (await browser.newContext(phone)).newPage();
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(url);
  return page;
}
const shot = (page, name) => SHOTS && page.screenshot({ path: `${SHOTS}/${name}.png` });
async function hold(page, ms) {
  const box = await page.locator('#open').boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await sleep(ms);
  await page.mouse.up();
}
const logged = (page, start, timeout = 8000) =>
  page.waitForFunction((t) => [...document.querySelectorAll('#log li .what')].some((li) => li.textContent.startsWith(t)), start, { timeout });

try {
  const page = await newPhone(sim.base);
  await page.waitForSelector('#claim-form:not([hidden])');
  await shot(page, '01-pair');
  await page.fill('#claim-name', 'Luis’s Pixel');
  await page.click('#claim-form button');
  await page.waitForFunction(() => document.querySelector('#status-text').textContent === 'On Wi-Fi');
  check('pairs as admin and connects', true);

  await sim.post('/sim/ring?line=lobby&ms=1500');
  await page.waitForSelector('#ring.now:not([hidden])');
  check('ring banner while it rings', (await page.textContent('#ring-title')).includes('lobby'));
  await shot(page, '02-ringing');
  await logged(page, 'Lobby rang');
  check('the press is logged with its length', (await page.locator('#log li .detail').first().textContent()).startsWith('held'));

  await hold(page, 250);
  await sleep(200);
  check('a tap doesn’t open', (await page.getAttribute('#open', 'data-state')) === 'idle');
  await hold(page, 900);
  await page.waitForSelector('#open[data-state="open"]', { timeout: 4000 });
  check('holding opens the door', (await page.textContent('#open-title')) === 'Door open');
  await shot(page, '03-open');
  await hold(page, 900);
  await page.waitForSelector('#open[data-state="error"]', { timeout: 4000 });
  check('a second opening during the cooldown is refused', (await page.textContent('#open-hint')).includes('just opened'));
  await sleep(4200);

  await page.click('#auto-chips [data-min="60"]');
  await sim.post('/sim/ring?line=lobby&ms=400');
  await logged(page, 'Opened by auto-open');
  check('auto-open opens on a ring', true);
  await page.click('#auto-chips [data-min="0"]');
  await page.click('#mute-chips [data-min="480"]');
  await page.waitForFunction(() => document.querySelector('#mute-info').textContent.includes('8 h'));
  check('mute shows its time left', true);
  await page.click('#mute-chips [data-min="0"]');

  await page.click('#settings summary');
  await page.waitForSelector('#admin:not([hidden])');
  await page.check('#code-on');
  for (let i = 0; i < 12; i++) await page.click('[data-sym="back"]');
  for (const sym of ['.', '.', '-']) await page.click(`[data-sym="${sym}"]`);
  await page.click('#code-form button[type="submit"]');
  await page.waitForFunction(() => document.querySelector('#toast').textContent === 'Saved.', null, { timeout: 4000 });
  await sleep(3500);
  await sim.post('/sim/code');
  await logged(page, 'Opened with the code');
  check('the lobby code opens the door', true);

  await page.fill('#key-name', 'Ana’s iPhone');
  await page.click('#key-form button');
  await page.waitForSelector('#new-key:not([hidden])');
  const link = await page.textContent('#links .link code');
  const ana = await newPhone(link);
  await ana.waitForFunction(() => document.querySelector('#status-text').textContent === 'On Wi-Fi');
  check('a second phone pairs from its link', (await ana.textContent('#toast')).includes('Ana’s iPhone'));
  await sleep(3200);
  await hold(ana, 900);
  await logged(page, 'Opened by Ana’s iPhone');
  check('the second phone opens, the first sees it', true);
  await ana.click('#settings summary');
  check('a non-admin gets no admin settings', await ana.isHidden('#admin'));

  page.once('dialog', (d) => d.accept());
  await page.locator('#keys li', { hasText: 'Ana' }).locator('button').click();
  await ana.waitForFunction(() => document.querySelector('#status-text').textContent === 'Key removed', null, { timeout: 8000 });
  check('a removed phone is locked out at once', await ana.isDisabled('#open'));

  check('no page errors', errors.length === 0, errors.join(' | '));
} finally {
  await browser.close();
  await sim.stop();
}
done('e2e');
