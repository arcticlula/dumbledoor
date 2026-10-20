import { fromBase64Url } from './crypto.js';
import { Dumbledoor, sign, storage } from './device.js';

const $ = (id) => document.getElementById(id);

const API_VERSION = 1;
const HOLD_MS = 700; // how long the open button has to be held
const PAIRING = 'dumbledoor.pairing';
const NOTIFY = 'dumbledoor.notify';
const RECENT_RING_MS = 2 * 60 * 1000; // a ring this recent still shows at the top

// The device serves this page over http:// at home; the copy hosted on https://
// (GitHub Pages) can only reach it through the broker. ?remote and ?local override.
const params = new URLSearchParams(location.search);
const mode = params.has('remote')
  ? 'remote'
  : params.has('local')
    ? 'local'
    : location.protocol === 'https:'
      ? 'remote'
      : 'local';

const ERRORS = {
  offline: 'Not connected to Dumbledoor.',
  timeout: 'No answer from Dumbledoor.',
  busy: 'It was just opened. Try again in a few seconds.',
  unknown_key: 'This phone’s key was removed. Ask an admin for a new link.',
  bad_signature: 'This phone’s key doesn’t match. Pair it again.',
  wrong_device: 'This key belongs to another Dumbledoor.',
  replay: 'Your phone’s clock seems to have gone back. Check its time.',
  clock: 'Your phone’s clock is off. Check its date and time.',
  bad_request: 'Dumbledoor couldn’t read that.',
  forbidden: 'Only admins can do that.',
  local_only: 'That only works from home Wi-Fi.',
  unknown_action: 'Dumbledoor’s firmware doesn’t know that yet. Update it.',
  invalid: 'Dumbledoor didn’t accept that. Check the values.',
  invalid_code: 'The code needs at least one press.',
  invalid_mqtt: 'Check the broker addresses: mqtts:// for Dumbledoor, wss:// for phones.',
  invalid_url: 'Addresses must start with https:// (or http://).',
  full: 'No room for more phones. Remove one first.',
  last_admin: 'That’s the only admin. Make another admin first.',
  not_found: 'That phone was already removed.',
  no_memory: 'Dumbledoor ran out of memory. Try again.',
  claim_closed: 'Pairing isn’t open right now.',
  no_pins: 'Dumbledoor’s GPIOs aren’t confirmed in its firmware yet, so it can’t open the door.',
  garbled: 'Dumbledoor’s answer didn’t make sense. Try again.',
};
const errorText = (code) => ERRORS[code] || `Something went wrong (${code}).`;

const WIFI_ERRORS = {
  2: 'wrong password?', // some routers drop a bad handshake with reason 2, not 15
  15: 'wrong password?',
  202: 'wrong password?',
  204: 'wrong password?',
  201: 'network not found',
  210: 'network not found',
  211: 'network not found',
};

let pairing = loadPairing();
let info = null; // home network only: /api/info
let device = null;
let seenEvent = 0; // newest event number already shown
let ringing = {}; // line → ringing, as of the last state
let firstState = true;

// Helpers ---------------------------------------------------------------------

function toast(text, ms = 3500) {
  const t = $('toast');
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => (t.hidden = true), ms);
}

function banner(text) {
  $('banner').hidden = !text;
  $('banner').textContent = text || '';
}

async function getJSON(url, options) {
  const res = await fetch(url, options);
  const data = await res.json().catch(() => ({}));
  if (!res.ok && data.ok === undefined) throw new Error(`HTTP ${res.status}`);
  return data;
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const bytes = (text) => new TextEncoder().encode(text).length;

function duration(seconds) {
  const s = Math.max(0, Math.round(seconds));
  if (s < 60) return `${s} s`;
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min`;
  const h = Math.floor(m / 60);
  return m % 60 ? `${h} h ${m % 60} min` : `${h} h`;
}

function ago(ms) {
  if (ms < 10000) return 'just now';
  if (ms < 60000) return `${Math.round(ms / 1000)} s ago`;
  if (ms < 3600000) return `${Math.round(ms / 60000)} min ago`;
  if (ms < 86400000) return `${Math.round(ms / 3600000)} h ago`;
  return new Date(Date.now() - ms).toLocaleString([], { weekday: 'short', hour: '2-digit', minute: '2-digit' });
}

const pressLength = (ms) => (ms < 1000 ? `${ms} ms` : `${(ms / 1000).toFixed(1)} s`);

// How long ago an event happened. The device stamps events with Unix time once NTP has
// answered; before that, only with its uptime.
function eventAge(e) {
  if (e.time) return Math.max(0, Date.now() - e.time);
  const s = device.state;
  return ((s.up - e.up) >>> 0) + (Date.now() - device.stateAt);
}

// Seconds left on a countdown the device reported when the state arrived.
const left = (seconds) => Math.max(0, seconds - (Date.now() - device.stateAt) / 1000);

// Pairing ---------------------------------------------------------------------

function validPairing(p) {
  return p && typeof p.d === 'string' && /^[0-9a-f]{8}$/.test(p.k) && /^[0-9a-f]{64}$/.test(p.s);
}

function loadPairing() {
  try {
    const p = JSON.parse(storage.get(PAIRING));
    return validPairing(p) ? p : null;
  } catch {
    return null;
  }
}

function savePairing(p) {
  pairing = p;
  storage.set(PAIRING, JSON.stringify(p));
}

// Links look like <app>#pair=<base64url JSON>. The fragment never reaches a server.
function importLink() {
  const match = location.hash.match(/^#pair=([\w-]+)$/);
  if (!match) return;
  history.replaceState(null, '', location.pathname + location.search);
  try {
    const p = JSON.parse(fromBase64Url(match[1]));
    if (!validPairing(p)) throw new Error('incomplete');
    savePairing(p);
    toast(`Paired with ${p.dn || 'Dumbledoor'} as “${p.n}”.`);
  } catch {
    toast('That pairing link is broken. Ask for a new one.', 6000);
  }
}

// Start -----------------------------------------------------------------------

async function start() {
  importLink();
  if ('serviceWorker' in navigator && window.isSecureContext) {
    navigator.serviceWorker.register('sw.js').catch(() => {});
  }
  if (mode === 'local') {
    try {
      info = await getJSON('api/info');
    } catch {
      $('status-text').textContent = 'Can’t reach it';
      $('status-dot').className = 'dot problem';
      banner('Can’t reach Dumbledoor. Is this phone on the same Wi-Fi? Trying again…');
      setTimeout(start, 4000);
      return;
    }
    banner('');
    if (info.v !== API_VERSION) banner('This app and Dumbledoor’s firmware don’t match. Update the firmware.');
    if (info.viaSetup) return showWifiSetup();
    if (!pairing || pairing.d !== info.id) return showPair();
  } else if (!pairing || !pairing.m) {
    return showPair();
  }
  showHome();
}

// Pairing screen ----------------------------------------------------------------

function showPair() {
  $('pair').hidden = false;
  $('home').hidden = true;
  $('status-text').textContent = 'Not paired';
  $('status-dot').className = 'dot';
  const help = $('pair-help');
  if (mode === 'remote') {
    $('claim-form').hidden = true;
    help.textContent = pairing
      ? 'This phone’s link has no broker details, so it only works at home: Dumbledoor’s remote access wasn’t set up when the link was made. Once Settings → Remote access says Dumbledoor is connected to the broker, make a new link (or ask an admin for one).'
      : 'Open the pairing link you were sent. Admins make one at home: open http://dumbledoor.local, then Settings → Phones.';
    return;
  }
  $('claim-form').hidden = !info.claim;
  if (info.claim) {
    help.textContent = '';
  } else if (info.paired) {
    help.textContent =
      'This Dumbledoor already has an admin. Ask them to add this phone (Settings → Phones), or hold the button on Dumbledoor’s board for 5 seconds to pair it as a new admin.';
  } else {
    help.textContent = 'Hold the button on Dumbledoor’s board for 5 seconds to pair this phone.';
  }
  // Holding the board's button opens pairing; notice it without a reload.
  clearTimeout(showPair.timer);
  showPair.timer = setTimeout(async () => {
    if ($('pair').hidden) return;
    try {
      info = await getJSON('api/info');
    } catch {}
    showPair();
  }, 3000);
}

$('claim-form').onsubmit = async (event) => {
  event.preventDefault();
  const name = $('claim-name').value.trim();
  if (bytes(name) > 23) return toast('That name is too long.');
  try {
    const reply = await getJSON('api/claim', { method: 'POST', body: JSON.stringify({ name }) });
    if (!reply.ok) throw new Error(reply.error);
    savePairing(reply.pair);
    $('pair').hidden = true;
    toast('Paired. This phone is the admin.');
    showHome();
  } catch (err) {
    toast(errorText(err.message));
  }
};

// Home ------------------------------------------------------------------------

function showHome() {
  $('pair').hidden = true;
  $('home').hidden = false;
  device = new Dumbledoor(pairing, mode);
  device.addEventListener('status', renderStatus);
  device.addEventListener('state', onState);
  device.start();
  renderStatus();
  renderPhone();
}

function renderStatus() {
  const s = device.status;
  const dot = $('status-dot');
  dot.className = `dot ${s === 'online' ? 'online' : s === 'connecting' ? 'connecting' : 'problem'}`;
  const text = {
    online: mode === 'local' ? 'On Wi-Fi' : 'Online',
    connecting: 'Connecting…',
    offline: device.detail === 'never_connected' ? 'Not on the broker' : 'Offline',
    denied: 'Key removed',
    error: 'Can’t sign in',
  }[s];
  $('status-text').textContent = text;
  if (s === 'denied' && mode === 'local') repairIfOpen();
  // Settings opened before the connection was up: fill them in now, before anyone saves
  // the empty forms over what Dumbledoor has.
  if (s === 'online' && !adminLoaded && $('settings').open && pairing.a) loadAdmin();
  if (s === 'denied') banner(errorText('unknown_key'));
  else if (s === 'error') banner(errorText(device.detail));
  else if (s === 'offline' && device.detail === 'never_connected')
    banner('Dumbledoor hasn’t connected to the broker yet. Check Remote access in its settings, at home.');
  else if (s === 'offline' && mode === 'remote' && device.detail)
    banner(`Can’t reach the broker: ${device.detail}. Trying again…`);
  else if (s === 'offline') banner(mode === 'local' ? 'Lost Dumbledoor. Trying again…' : 'Dumbledoor is offline.');
  else banner(stateWarning());
  renderOpen();
  renderPhone();
}

// Problems the state itself reports, shown while everything else is fine.
function stateWarning() {
  const s = device?.state;
  if (!s) return '';
  if (s.v !== API_VERSION) return 'This app and Dumbledoor’s firmware don’t match. Update the firmware.';
  if (s.pins === false)
    return 'Dumbledoor’s firmware doesn’t have its GPIOs confirmed yet (menuconfig → Dumbledoor → Intercom lines), so it won’t open the door or ring the monitor.';
  return '';
}

// This phone's key is gone. If the device is taking a new admin (it was wiped, or its
// button was held), go straight to pairing instead of a dead end.
async function repairIfOpen() {
  try {
    info = await getJSON('api/info');
  } catch {
    return;
  }
  if (!info.claim) return;
  device.stop();
  storage.remove(PAIRING);
  pairing = null;
  banner('');
  showPair();
}

function onState() {
  const s = device.state;
  if (device.status === 'online') banner(stateWarning());
  $('title').textContent = s.name || 'Dumbledoor';
  document.title = s.name || 'Dumbledoor';

  // A ring that starts while we're watching (not a stale one in the first state).
  for (const line of s.lines || []) {
    const now = !!s.ringing?.[line];
    if (now && !ringing[line] && !firstState) ringStarted(line);
    ringing[line] = now;
  }
  // With the lobby code on, lobby presses only show up once they stop, as one outcome.
  const fresh = firstState ? [] : (s.events || []).filter((e) => e.n > seenEvent);
  if (s.code?.on && fresh.some(atLobby)) ringStarted('lobby');
  renderLog();
  renderRing();
  renderToggles();
  renderOpen();
  renderDevice();
  firstState = false;
}

// The ring banner and notifications ------------------------------------------

const RING_TEXT = { lobby: 'Someone’s at the lobby', front: 'Someone’s at your front door' };
// Events that mean someone was at a door: a ring, or a wrong code at the lobby panel.
const atDoor = (e) => e.e === 'ring' || e.e === 'code_fail';
const atLobby = (e) => (e.e === 'ring' && e.line === 'lobby') || e.e === 'code_fail';

function ringStarted(line) {
  navigator.vibrate?.([200, 100, 200, 100, 200]);
  if (document.hidden && storage.get(NOTIFY) === '1' && window.Notification?.permission === 'granted') {
    navigator.serviceWorker?.ready.then((reg) =>
      reg.showNotification(RING_TEXT[line] || 'Someone rang', {
        body: line === 'lobby' ? 'Open Dumbledoor to let them in.' : 'Someone rang at your door.',
        tag: 'ring',
        renotify: true,
        icon: 'icon-192.png',
        vibrate: [200, 100, 200],
      }),
    );
  }
}

function renderRing() {
  const s = device?.state;
  const card = $('ring');
  if (!s) return (card.hidden = true);
  let line = (s.lines || []).find((l) => s.ringing?.[l]);
  const now = !!line;
  let sub = 'Ringing now';
  if (!now) {
    const events = s.events || [];
    const ring = [...events].reverse().find(atDoor);
    if (!ring || eventAge(ring) > RECENT_RING_MS) return (card.hidden = true);
    line = ring.line || 'lobby';
    const opened = events.some((e) => e.e === 'open' && e.n > ring.n);
    sub = `${ring.e === 'code_fail' ? 'Wrong code' : 'Rang'} ${ago(eventAge(ring))}${opened ? ' · door opened' : ''}`;
  }
  card.hidden = false;
  card.classList.toggle('now', now);
  $('ring-title').textContent = RING_TEXT[line] || 'Someone rang';
  $('ring-sub').textContent = sub;
}

// The open button --------------------------------------------------------------

const button = $('open');
let phase = 'idle'; // idle, holding, sending, open, error
let holdStart = 0;
let frame = 0;
let phaseTimer = 0;

function setPhase(next, title, hint) {
  phase = next;
  clearTimeout(phaseTimer);
  button.dataset.state = next;
  if (next !== 'holding') button.style.setProperty('--progress', 0);
  if (title !== undefined) {
    $('open-title').textContent = title;
    $('open-hint').textContent = hint;
  }
  document.body.classList.toggle('door-open', next === 'open');
}

function renderOpen() {
  const online = device?.status === 'online';
  button.disabled = !online;
  if (phase === 'holding' || phase === 'sending' || phase === 'error') return;
  if (phase === 'open' && phaseTimer) return; // our own opening, still showing
  if (online && device.state?.opening) {
    setPhase('open', 'Door open', 'Someone just opened it');
  } else {
    setPhase('idle', online ? 'Hold to open' : 'Not connected', 'Opens the lobby door downstairs');
  }
}

function startHold(event) {
  if (button.disabled || phase === 'sending') return;
  event.preventDefault();
  if (event.pointerId !== undefined) button.setPointerCapture(event.pointerId);
  setPhase('holding', 'Keep holding…', 'Opens the lobby door downstairs');
  holdStart = performance.now();
  const tick = () => {
    const progress = Math.min(1, (performance.now() - holdStart) / HOLD_MS);
    button.style.setProperty('--progress', progress);
    if (progress < 1) frame = requestAnimationFrame(tick);
    else openDoor();
  };
  frame = requestAnimationFrame(tick);
}

function cancelHold() {
  if (phase !== 'holding') return;
  cancelAnimationFrame(frame);
  setPhase('idle');
  renderOpen();
}

async function openDoor() {
  navigator.vibrate?.(30);
  setPhase('sending', 'Opening…', '');
  const reply = await device.send('open');
  if (reply.ok) {
    setPhase('open', 'Door open', 'Alohomora ✨');
    phaseTimer = setTimeout(() => {
      phaseTimer = 0;
      setPhase('idle');
      renderOpen();
    }, 3000);
  } else {
    navigator.vibrate?.([60, 60, 60]);
    setPhase('error', 'Couldn’t open', errorText(reply.error));
    phaseTimer = setTimeout(() => {
      setPhase('idle');
      renderOpen();
    }, 4000);
  }
}

button.addEventListener('pointerdown', startHold);
button.addEventListener('pointerup', cancelHold);
button.addEventListener('pointercancel', cancelHold);
button.addEventListener('lostpointercapture', cancelHold);
button.addEventListener('contextmenu', (event) => event.preventDefault());
button.addEventListener('keydown', (event) => {
  if ((event.key === ' ' || event.key === 'Enter') && !event.repeat) startHold(event);
});
button.addEventListener('keyup', (event) => {
  if (event.key === ' ' || event.key === 'Enter') cancelHold();
});

// Auto-open and mute -----------------------------------------------------------

const AUTO_PRESETS = [15, 60, 180];
const MUTE_PRESETS = [60, 480];

function highlight(chips, value) {
  for (const chip of chips.children) chip.setAttribute('aria-pressed', String(Number(chip.dataset.min) === value));
}

// The preset that best matches what's left, so every phone shows the same choice.
const preset = (presets, minutesLeft) => presets.find((p) => minutesLeft <= p) ?? presets[presets.length - 1];

function renderToggles() {
  const s = device?.state;
  if (!s) return;
  const autoLeft = left(s.auto || 0);
  highlight($('auto-chips'), autoLeft > 0 ? preset(AUTO_PRESETS, autoLeft / 60) : 0);
  $('auto-info').textContent = autoLeft > 0 ? `on, ${duration(autoLeft)} left` : '';

  $('mute-field').hidden = !s.bell;
  const muteLeft = s.mute > 0 ? left(s.mute) : 0;
  const muted = s.mute < 0 ? -1 : muteLeft > 0 ? preset(MUTE_PRESETS, muteLeft / 60) : 0;
  highlight($('mute-chips'), muted);
  $('mute-info').textContent = s.mute < 0 ? 'muted' : muteLeft > 0 ? `muted, ${duration(muteLeft)} left` : '';
}

async function toggle(action, chip) {
  if (!chip?.dataset.min) return;
  const min = Number(chip.dataset.min);
  const reply = await device.send(action, { min });
  if (!reply.ok) toast(errorText(reply.error));
  else if (action === 'auto' && min > 0) toast(`Every lobby ring opens the door for the next ${duration(min * 60)}.`);
}

$('auto-chips').onclick = (event) => toggle('auto', event.target.closest('.chip'));
$('mute-chips').onclick = (event) => toggle('mute', event.target.closest('.chip'));

// Activity ---------------------------------------------------------------------

const VIA = { local: 'from home Wi-Fi', remote: 'from outside', panel: '', auto: '' };
const REASONS = { 'power-on': 'power on', restart: 'restart', crash: 'after a crash', watchdog: 'after it froze', brownout: 'after a power dip' };
const DENIED = { unknown_key: 'A removed or unknown phone', bad_signature: 'A forged or broken command', wrong_device: 'A key for another Dumbledoor' };
const PRESS = { '.': '•', '-': '—', '?': '?' }; // short, long, in between
const LOG_SHORT = 6; // events shown until "Show all"
let logExpanded = false;

function describe(e) {
  switch (e.e) {
    case 'ring':
      return ['🔔', e.line === 'front' ? 'Front door rang' : 'Lobby rang', `held ${pressLength(e.ms)}`];
    case 'open':
      if (e.via === 'panel') return ['🚪', 'Opened with the code', ''];
      if (e.via === 'auto') return ['🚪', 'Opened by auto-open', ''];
      return ['🚪', `Opened by ${e.by}`, VIA[e.via] ?? ''];
    case 'auto':
      return ['⏱️', e.min ? `Auto-open on for ${duration(e.min * 60)}` : 'Auto-open off', `by ${e.by}`];
    case 'mute':
      if (e.min === 0) return ['🔔', 'The monitor rings again', `by ${e.by}`];
      return ['🔕', e.min < 0 ? 'Monitor muted' : `Monitor muted for ${duration(e.min * 60)}`, `by ${e.by}`];
    case 'code_fail': {
      const presses = [...(e.seq || '')].map((p) => PRESS[p] || p).join(' ');
      return ['✋', 'Wrong code at the panel', [presses, e.count > 1 ? `${e.count} in a row` : ''].filter(Boolean).join(' · ')];
    }
    case 'denied':
      return ['⚠️', 'Refused a command', `${DENIED[e.why] || e.why} ${VIA[e.via] ?? ''}`.trim()];
    case 'key_add':
      return ['📱', `Added ${e.name}`, ''];
    case 'key_remove':
      return ['📱', `Removed ${e.name}`, ''];
    case 'boot':
      return ['⚡', 'Dumbledoor started', REASONS[e.reason] || e.reason];
    default:
      return ['•', e.e, ''];
  }
}

function renderLog() {
  const events = [...(device.state.events || [])].reverse();
  $('log-empty').hidden = events.length > 0;
  $('log-more').hidden = events.length <= LOG_SHORT;
  $('log-more').textContent = logExpanded ? 'Show less' : `Show all ${events.length}`;
  $('log').replaceChildren(
    ...events.slice(0, logExpanded ? events.length : LOG_SHORT).map((e) => {
      const [icon, what, detail] = describe(e);
      const li = document.createElement('li');
      li.dataset.n = e.n;
      if (!firstState && e.n > seenEvent) li.className = 'new';
      const iconEl = document.createElement('span');
      iconEl.className = 'icon';
      iconEl.textContent = icon;
      const whatEl = document.createElement('span');
      whatEl.className = 'what';
      whatEl.textContent = what;
      if (detail) {
        const d = document.createElement('span');
        d.className = 'detail';
        d.textContent = detail;
        whatEl.append(d);
      }
      const when = document.createElement('span');
      when.className = 'when';
      when.textContent = ago(eventAge(e));
      li.append(iconEl, whatEl, when);
      return li;
    }),
  );
  seenEvent = Math.max(seenEvent, ...events.map((e) => e.n), 0);
}

$('log-more').onclick = () => {
  logExpanded = !logExpanded;
  renderLog();
};

function refreshTimes() {
  if (!device?.state || document.hidden) return;
  const byN = new Map((device.state.events || []).map((e) => [String(e.n), e]));
  for (const li of $('log').children) {
    const e = byN.get(li.dataset.n);
    if (e) li.querySelector('.when').textContent = ago(eventAge(e));
  }
  renderRing();
  renderToggles();
}
setInterval(refreshTimes, 1000);

let hiddenAt = 0;
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    hiddenAt = Date.now();
  } else {
    device?.wake(Date.now() - hiddenAt > 30000);
    refreshTimes();
  }
});

// Settings: this phone ------------------------------------------------------------

function renderPhone() {
  if (!pairing) return;
  // A phone from an ordinary link only gets the button.
  document.body.classList.toggle('guest', !pairing.a);
  $('me-key').textContent = `${pairing.n}${pairing.a ? ' · admin' : ''}`;
  $('me-link').textContent = mode === 'local' ? `home Wi-Fi (${location.host})` : 'through the broker';
  $('settings-summary').textContent = pairing.dn || '';
  const canNotify = mode === 'remote' && window.isSecureContext && 'Notification' in window && 'serviceWorker' in navigator;
  $('notify-row').hidden = !canNotify;
  $('notify').checked = canNotify && storage.get(NOTIFY) === '1' && Notification.permission === 'granted';
  // Admins can change the lobby code from anywhere; phones and the other settings only at home.
  const home = mode === 'local';
  for (const group of document.querySelectorAll('#admin .home-only')) group.hidden = !home;
  $('admin-hint').hidden = home;
}

$('notify').onchange = async (event) => {
  if (!event.target.checked) return storage.remove(NOTIFY);
  const permission = await Notification.requestPermission();
  if (permission === 'granted') {
    storage.set(NOTIFY, '1');
    toast('You’ll be notified while the app is open in the background. For a closed app, set up ntfy.', 6000);
  } else {
    event.target.checked = false;
    toast('Notifications are blocked for this site in the browser’s settings.');
  }
};

$('forget').onclick = () => {
  if (!confirm('Forget this Dumbledoor on this phone? You’ll need a new pairing link to use it again.')) return;
  storage.remove(PAIRING);
  device?.stop();
  location.reload();
};

// Settings: admin (the lobby code from anywhere, the rest at home) -------------------

let settings = null;
let codeSeq = '';
let adminLoaded = false; // the admin forms hold what Dumbledoor has

$('settings').addEventListener('toggle', () => {
  if ($('settings').open && pairing?.a) loadAdmin();
});

async function loadAdmin() {
  const home = mode === 'local';
  const [c, s, k] = await Promise.all([device.code(), home && device.send('settings'), home && device.send('keys')]);
  adminLoaded = c.ok && (!home || (s.ok && k.ok));
  if (c.ok) fillCode(c.code);
  else toast(errorText(c.error));
  if (!home) return;
  if (s.ok) {
    settings = s.settings;
    fillSettings();
  } else {
    toast(errorText(s.error));
  }
  if (k.ok) renderKeys(k.keys);
}

function renderKeys(keys) {
  $('keys').replaceChildren(
    ...keys.map((k) => {
      const li = document.createElement('li');
      const name = document.createElement('span');
      name.className = 'name';
      name.textContent = k.name;
      if (k.admin) {
        const b = document.createElement('span');
        b.className = 'badge';
        b.textContent = 'admin';
        name.append(b);
      }
      const last = document.createElement('span');
      last.className = 'detail muted';
      last.textContent = k.id === pairing.k ? ' · this phone' : k.last ? ` · used ${ago(Date.now() - k.last)}` : ' · never used';
      name.append(last);
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.textContent = 'Remove';
      remove.onclick = async () => {
        const self = k.id === pairing.k;
        if (!confirm(self ? 'Remove this phone’s own key? It stops working right away.' : `Remove ${k.name}? Their phone stops working right away.`)) return;
        const reply = await device.send('key_remove', { id: k.id });
        if (!reply.ok) return toast(errorText(reply.error));
        const list = await device.send('keys');
        if (list.ok) renderKeys(list.keys);
      };
      li.append(name, remove);
      return li;
    }),
  );
}

$('key-form').onsubmit = async (event) => {
  event.preventDefault();
  const name = $('key-name').value.trim();
  if (!name || bytes(name) > 23) return toast('Use a shorter name.');
  const reply = await device.send('key_add', { name, admin: $('key-admin').checked });
  if (!reply.ok) return toast(errorText(reply.error));
  $('key-name').value = '';
  $('key-admin').checked = false;
  showLinks(name, reply);
  const list = await device.send('keys');
  if (list.ok) renderKeys(list.keys);
};

// What Remote access still needs before Dumbledoor makes links for the online app (the
// same rule as the firmware's), or '' when nothing is missing.
function remoteGap(s) {
  if (!s) return 'set up Remote access';
  if (!s.mqtt.on) return 'turn on “Use remote access” and save';
  if (!s.mqtt.appUrl) return 'fill in “Broker, for phones” (for EMQX: wss://<address>:8084/mqtt) and save';
  if (!s.appUrl) return 'fill in “Where the online app is” and save';
  return '';
}

function showLinks(name, reply) {
  const links = [];
  if (reply.link) links.push(['For the online app (works anywhere)', reply.link]);
  if (reply.localLink) {
    const gap = remoteGap(settings);
    links.push([reply.link ? 'For home Wi-Fi only' : `Works on home Wi-Fi only. For a link that works anywhere, ${gap || 'set up Remote access'}, then make a new one.`, reply.localLink]);
  }
  $('links').replaceChildren(
    ...links.map(([label, url]) => {
      const box = document.createElement('div');
      box.className = 'link';
      const title = document.createElement('strong');
      title.textContent = label;
      const code = document.createElement('code');
      code.textContent = url;
      const actions = document.createElement('div');
      actions.className = 'chips';
      const copy = document.createElement('button');
      copy.type = 'button';
      copy.className = 'chip';
      copy.textContent = 'Copy';
      copy.onclick = async () => {
        try {
          await navigator.clipboard.writeText(url);
          toast('Copied.');
        } catch {
          prompt('Copy this link:', url);
        }
      };
      actions.append(copy);
      if (navigator.share) {
        const share = document.createElement('button');
        share.type = 'button';
        share.className = 'chip';
        share.textContent = 'Share';
        share.onclick = () => navigator.share({ title: 'Dumbledoor', text: `Your Dumbledoor key, “${name}”:`, url }).catch(() => {});
        actions.append(share);
      }
      const open = document.createElement('a');
      open.className = 'chip';
      open.href = url;
      open.target = '_blank';
      open.rel = 'noopener';
      open.textContent = 'Open here';
      actions.append(open);
      box.append(title, code, actions);
      return box;
    }),
  );
  $('new-key').hidden = false;
}

function fillCode(code) {
  $('code-on').checked = code.on;
  codeSeq = code.seq;
  $('code-short').value = code.short;
  $('code-long').value = code.long;
  $('code-gap').value = code.gap;
  renderCode();
}

function fillSettings() {
  const s = settings;
  $('mqtt-on').checked = s.mqtt.on;
  $('mqtt-url').value = s.mqtt.url;
  $('mqtt-user').value = s.mqtt.user;
  $('mqtt-pass').value = '';
  $('mqtt-pass').placeholder = s.mqtt.hasPass ? 'Password (saved, leave empty to keep)' : 'Password';
  $('app-mqtt-url').value = s.mqtt.appUrl;
  $('app-mqtt-user').value = s.mqtt.appUser;
  $('app-mqtt-pass').value = '';
  $('app-mqtt-pass').placeholder = s.mqtt.hasAppPass ? 'Password (saved, leave empty to keep)' : 'Password';
  $('mqtt-prefix').value = s.mqtt.prefix;
  $('app-url').value = s.appUrl;
  $('ntfy').value = s.ntfy;
  $('device-name').value = s.name;
  renderDevice();
}

function renderCode() {
  const view = $('code-view');
  if (!codeSeq) {
    view.textContent = 'No code yet: add presses below';
  } else {
    view.replaceChildren(
      ...[...codeSeq].map((sym) => {
        const mark = document.createElement('span');
        mark.className = sym === '-' ? 'long' : 'short';
        return mark;
      }),
    );
    view.setAttribute('aria-label', [...codeSeq].map((sym) => (sym === '-' ? 'long' : 'short')).join(', '));
  }
  $('code-short-value').textContent = pressLength(Number($('code-short').value));
  $('code-long-value').textContent = pressLength(Number($('code-long').value));
  $('code-gap-value').textContent = pressLength(Number($('code-gap').value));
}

document.querySelector('.code-editor .chips').onclick = (event) => {
  const sym = event.target.closest('.chip')?.dataset.sym;
  if (!sym) return;
  if (sym === 'back') codeSeq = codeSeq.slice(0, -1);
  else if (codeSeq.length < 12) codeSeq += sym;
  renderCode();
};
// Short ends at or before long starts: moving one past the other pushes it along.
$('code-short').oninput = () => {
  if (Number($('code-short').value) > Number($('code-long').value)) $('code-long').value = $('code-short').value;
  renderCode();
};
$('code-long').oninput = () => {
  if (Number($('code-long').value) < Number($('code-short').value)) $('code-short').value = $('code-long').value;
  renderCode();
};
$('code-gap').oninput = renderCode;

async function saveSettings(set, done = 'Saved.') {
  const reply = await device.send('settings', { set });
  if (!reply.ok) return toast(errorText(reply.error));
  settings = reply.settings;
  fillSettings();
  if (typeof done === 'function') toast(done(settings), 6000);
  else toast(done);
}

$('code-form').onsubmit = async (event) => {
  event.preventDefault();
  if ($('code-on').checked && codeSeq.length < 3 && !confirm('A code this short is easy to guess. Use it anyway?')) return;
  const reply = await device.code({
    on: $('code-on').checked,
    seq: codeSeq,
    short: Number($('code-short').value),
    long: Number($('code-long').value),
    gap: Number($('code-gap').value),
  });
  if (!reply.ok) return toast(errorText(reply.error));
  fillCode(reply.code);
  toast('Saved.');
};

$('remote-form').onsubmit = (event) => {
  event.preventDefault();
  saveSettings(
    {
      mqtt: {
        on: $('mqtt-on').checked,
        url: $('mqtt-url').value.trim(),
        user: $('mqtt-user').value.trim(),
        pass: $('mqtt-pass').value,
        prefix: $('mqtt-prefix').value.trim(),
        appUrl: $('app-mqtt-url').value.trim(),
        appUser: $('app-mqtt-user').value.trim(),
        appPass: $('app-mqtt-pass').value,
      },
      appUrl: $('app-url').value.trim(),
    },
    (s) => {
      const gap = remoteGap(s);
      return gap ? `Saved, but new links still only work at home: ${gap}.` : 'Saved. Links you make from now on work from anywhere.';
    },
  );
};

$('push-form').onsubmit = (event) => {
  event.preventDefault();
  saveSettings({ ntfy: $('ntfy').value.trim() });
};

$('device-form').onsubmit = (event) => {
  event.preventDefault();
  saveSettings({ name: $('device-name').value.trim() });
};

function renderDevice() {
  const s = device?.state;
  if (!s) return;
  $('fw').textContent = `${s.fw} · ${s.id}`;
  $('wifi-info').textContent = s.wifi?.ssid ? `${s.wifi.ssid} · ${s.wifi.rssi} dBm` : 'not connected';
  $('remote-status').textContent = !s.remote?.on
    ? 'Remote access is off.'
    : s.remote.ok
      ? 'Dumbledoor is connected to the broker.'
      : 'Dumbledoor isn’t connected to the broker (yet).';
}

$('fw-file').onchange = () => {
  const file = $('fw-file').files[0];
  if (!file) return;
  if (!confirm(`Install ${file.name} on Dumbledoor? It restarts afterwards, and goes back to the old firmware if the new one crashes.`)) {
    $('fw-file').value = '';
    return;
  }
  const { text } = sign(pairing, 'ota');
  const xhr = new XMLHttpRequest();
  xhr.open('POST', 'api/ota');
  xhr.setRequestHeader('X-Auth', text);
  const progress = $('fw-progress');
  progress.hidden = false;
  $('fw-msg').textContent = 'Uploading…';
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) progress.value = e.loaded / e.total;
  };
  xhr.onload = () => {
    progress.hidden = true;
    $('fw-file').value = '';
    let reply = {};
    try {
      reply = JSON.parse(xhr.responseText);
    } catch {}
    $('fw-msg').textContent = reply.ok
      ? 'Installed. Dumbledoor is restarting and the app will reconnect.'
      : `Update failed: ${reply.error === 'invalid_image' ? 'that isn’t Dumbledoor firmware for this board' : errorText(reply.error || xhr.status)}`;
  };
  xhr.onerror = () => {
    progress.hidden = true;
    $('fw-msg').textContent = 'Upload failed. Check the connection and try again.';
  };
  xhr.send(file);
};

$('reboot').onclick = async () => {
  if (!confirm('Restart Dumbledoor? It takes a few seconds.')) return;
  const reply = await device.send('reboot');
  toast(reply.ok ? 'Restarting…' : errorText(reply.error));
};

// Wi-Fi setup, on the device's hotspot ---------------------------------------------

let wifi = { host: 'dumbledoor.local', setupSsid: 'Dumbledoor Setup' };

function showWifiSetup() {
  $('wifi-setup').hidden = false;
  $('home').hidden = true;
  $('pair').hidden = true;
  $('status-text').textContent = 'Setup';
  $('status-dot').className = 'dot connecting';
  scan();
}

async function loadWifi() {
  try {
    wifi = await getJSON('api/wifi');
    return wifi;
  } catch {
    return null;
  }
}

async function scan() {
  const select = $('ssid');
  try {
    const list = await getJSON('api/scan');
    list.sort((a, b) => b.rssi - a.rssi);
    select.replaceChildren(new Option(list.length ? 'Choose your network' : 'No networks found', ''));
    for (const ap of list) select.add(new Option(ap.ssid, ap.ssid));
  } catch {
    select.replaceChildren(new Option('Couldn’t look for networks', ''));
  }
  select.add(new Option('Other network…', '__other'));
}

$('ssid').onchange = () => {
  const other = $('ssid').value === '__other';
  $('ssid-other').hidden = !other;
  $('ssid-other').required = other;
};

$('wifi-form').onsubmit = async (event) => {
  event.preventDefault();
  const ssid = $('ssid').value === '__other' ? $('ssid-other').value.trim() : $('ssid').value;
  if (!ssid) return;
  const msg = $('wifi-msg');
  msg.textContent = `Connecting to ${ssid}…`;
  try {
    const reply = await getJSON('api/wifi', {
      method: 'POST',
      body: JSON.stringify({ ssid, password: $('wifi-password').value }),
    });
    if (!reply.ok) throw new Error(reply.error);
  } catch {
    msg.textContent = 'That network name or password isn’t valid. Try again.';
    return;
  }
  // When Dumbledoor joins, its hotspot may change channel and drop this phone for a moment.
  for (let i = 0; i < 20; i++) {
    await sleep(1500);
    const w = await loadWifi();
    if (!w) continue;
    if (w.connected && w.ssid === ssid) {
      msg.textContent = `Done! Switch this phone back to ${ssid} and open http://${w.host} to pair it.`;
      return;
    }
    if (i >= 4 && WIFI_ERRORS[w.error]) {
      msg.textContent = `Couldn’t connect to ${ssid} (${WIFI_ERRORS[w.error]}). Check it and try again.`;
      return;
    }
  }
  msg.textContent = `If the “${wifi.setupSsid}” Wi-Fi disappears, it worked: switch back to ${ssid} and open http://${wifi.host}. Otherwise check the password and try again.`;
};

start();
