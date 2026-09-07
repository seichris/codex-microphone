import { randomBytes } from 'node:crypto';
import { access, mkdir, open, readFile, rename, rm } from 'node:fs/promises';
import { constants } from 'node:fs';
import { hostname } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const configPath = resolve(process.env.CODEX_ATTENTION_CONFIG ?? resolve(here, '..', 'config.json'));
const arguments_ = process.argv.slice(2);
if (arguments_.some((arg) => arg !== '--rotate-admin') || arguments_.length > 1) {
  throw new Error('Usage: npm run setup -- [--rotate-admin]');
}
const rotate = arguments_.includes('--rotate-admin');
let exists = false;
try { await access(configPath, constants.F_OK); exists = true; }
catch (error) { if (error?.code !== 'ENOENT') throw error; }

if (exists && !rotate) {
  console.error('Configuration already exists. Stop the bridge and use npm run setup -- --rotate-admin to migrate its administrative credential.');
  process.exitCode = 1;
} else {
  let previous = {};
  if (exists) {
    try {
      previous = JSON.parse(await readFile(configPath, 'utf8'));
      if (!previous || typeof previous !== 'object' || Array.isArray(previous)) throw new Error();
    } catch { throw new Error('Invalid existing configuration; nothing was overwritten.'); }
  }
  const config = {
    deviceHost: '0.0.0.0', devicePort: 5182, fallbackHost: hostname(), port: 5180,
    pollIntervalMs: 2000, maxThreads: 300, maxItems: 30,
    attentionFilter: 'unread+pinned', codexBin: 'codex', codexHome: '~/.codex',
    ...previous, host: '127.0.0.1', token: randomBytes(32).toString('base64url'),
  };
  await mkdir(dirname(configPath), { recursive: true });
  const temporary = `${configPath}.${randomBytes(8).toString('hex')}.tmp`;
  let file;
  try {
    file = await open(temporary, 'wx', 0o600);
    await file.writeFile(`${JSON.stringify(config, null, 2)}\n`);
    await file.sync(); await file.close(); file = null;
    await rename(temporary, configPath);
    const directory = await open(dirname(configPath), 'r');
    try { await directory.sync(); } finally { await directory.close(); }
  } finally { await file?.close(); await rm(temporary, { force: true }); }
  console.log(rotate ? 'Administrative credential rotated. Restart the bridge and Mac companion.' : 'Created owner-only bridge configuration.');
  console.log('Credentials are never printed, copied into URLs, or embedded in attention firmware.');
  console.log('Start the bridge, then run npm run pair -- --port <serial-device>.');
}
