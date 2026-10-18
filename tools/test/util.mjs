// Shared by the tests: a throwaway simulator, and a tiny check() reporter.
import { spawn } from 'node:child_process';
import { rm } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const sim = join(dirname(fileURLToPath(import.meta.url)), '..', 'sim', 'sim.mjs');

let failures = 0;
export function check(name, ok, detail = '') {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ` — ${detail}` : ''}`);
  if (!ok) failures++;
}
export function done(label) {
  console.log(failures ? `${label}: ${failures} failure(s)` : `${label}: all good`);
  process.exit(failures ? 1 : 0);
}

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Starts tools/sim/sim.mjs on a random port with no saved state. Resolves once it answers.
export async function startSim(extra = []) {
  const port = 20000 + Math.floor(Math.random() * 20000);
  const child = spawn(process.execPath, [sim, '--fresh', '--port', String(port), ...extra], { stdio: ['ignore', 'pipe', 'inherit'] });
  const base = `http://localhost:${port}`;
  for (let i = 0; i < 50; i++) {
    try {
      await fetch(`${base}/api/info`);
      break;
    } catch {
      await sleep(100);
    }
  }
  return {
    base,
    port,
    post: (path, body) => fetch(base + path, { method: 'POST', body }).then((r) => r.json()),
    async stop() {
      child.kill();
      await rm(join(dirname(sim), `.sim-state-${port}.json`), { force: true });
    },
  };
}
