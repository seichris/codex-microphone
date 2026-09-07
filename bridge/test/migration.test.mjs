import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, writeFile, rm, stat } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const run = promisify(execFile);
const setup = new URL('../src/setup.mjs', import.meta.url).pathname;
test('explicit admin migration rotates privately and preserves microphone/unrelated settings', async (t) => {
  const dir = await mkdtemp(join(tmpdir(), 'attention-migration-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const path = join(dir, 'config.json');
  const previous = { host: '0.0.0.0', token: 'old-compiled-admin-secret-12345', port: 5180,
    codexHome: '/custom', attentionFilter: 'all', microphone: { keep: true } };
  await writeFile(path, JSON.stringify(previous));
  const env = { ...process.env, CODEX_ATTENTION_CONFIG: path };
  await assert.rejects(run(process.execPath, [setup], { env }));
  assert.deepEqual(JSON.parse(await readFile(path)), previous);
  const output = await run(process.execPath, [setup, '--rotate-admin'], { env });
  const migrated = JSON.parse(await readFile(path));
  assert.notEqual(migrated.token, previous.token);
  assert.equal(migrated.host, '127.0.0.1');
  assert.equal(migrated.devicePort, 5182);
  assert.deepEqual(migrated.microphone, previous.microphone);
  assert.equal(migrated.attentionFilter, 'all');
  assert.equal(migrated.codexHome, '/custom');
  assert.equal((await stat(path)).mode & 0o777, 0o600);
  for (const secret of [previous.token, migrated.token]) assert(!`${output.stdout}${output.stderr}`.includes(secret));
  assert(!output.stdout.includes('#token='));
});

test('fresh setup is owner-only; invalid migration never overwrites existing config', async (t) => {
  const dir = await mkdtemp(join(tmpdir(), 'attention-new-config-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const path = join(dir, 'config.json'), env = { ...process.env, CODEX_ATTENTION_CONFIG: path };
  await run(process.execPath, [setup], { env });
  const config = JSON.parse(await readFile(path));
  assert.equal(config.host, '127.0.0.1'); assert(config.token.length >= 32);
  assert.equal((await stat(path)).mode & 0o777, 0o600);
  await writeFile(path, '{invalid-secret');
  await assert.rejects(run(process.execPath, [setup, '--rotate-admin'], { env }));
  assert.equal(await readFile(path, 'utf8'), '{invalid-secret');
});

test('legacy LAN host and URL do not become device/admin transport settings', async (t) => {
  const dir = await mkdtemp(join(tmpdir(), 'attention-legacy-config-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const path = join(dir, 'config.json');
  await writeFile(path, JSON.stringify({ host: '0.0.0.0', token: 'test-admin-token-only-123456789',
    bridgeUrl: 'http://old-lan/api/v1/attention', fallbackHost: '' }));
  const module = new URL('../src/config.mjs', import.meta.url).href;
  const { stdout } = await run(process.execPath, ['--input-type=module', '-e',
    `import { loadConfig } from ${JSON.stringify(module)}; const c = await loadConfig(); console.log(JSON.stringify({host:c.host,devicePort:c.devicePort,fallback:c.fallbackHost}));`],
  { env: { ...process.env, CODEX_ATTENTION_CONFIG: path, CODEX_ATTENTION_HOST: '0.0.0.0' } });
  assert.deepEqual(JSON.parse(stdout), { host: '127.0.0.1', devicePort: 5182, fallback: '' });
});
