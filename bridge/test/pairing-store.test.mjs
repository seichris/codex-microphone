import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, writeFile, rm, stat } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { PairingStore } from '../src/pairing-store.mjs';
import { randomHex } from '../src/device-auth.mjs';

async function fixture(t) {
  const directory = await mkdtemp(join(tmpdir(), 'attention-store-'));
  const store = await PairingStore.open(directory);
  t.after(async () => { await store.close(); await rm(directory, { recursive: true, force: true }); });
  return { directory, store };
}

test('identity, pending pairing and revocation persist with private permissions across restart', async (t) => {
  const { directory, store } = await fixture(t);
  const id = randomHex(16), paired = await store.prepare(id);
  assert.equal(store.device(id).status, 'pending');
  assert.equal((await stat(directory)).mode & 0o777, 0o700);
  assert.equal((await stat(join(directory, 'pairing.json'))).mode & 0o777, 0o600);
  await store.close();
  const restarted = await PairingStore.open(directory);
  assert.equal(restarted.bridgeId, paired.bridgeId);
  assert.equal(restarted.certificate, paired.certificate);
  assert.equal(restarted.device(id).secret, paired.secret);
  await restarted.activate(id);
  await restarted.revoke(id);
  await restarted.close();
  const again = await PairingStore.open(directory);
  assert.equal(again.device(id).status, 'revoked');
  assert(!JSON.stringify(again.list()).includes(paired.secret));
  await again.close();
});

test('re-pair activation atomically revokes old and competing pending credentials', async (t) => {
  const { store } = await fixture(t);
  const old = randomHex(16), first = randomHex(16), second = randomHex(16);
  await store.prepare(old); await store.activate(old);
  assert.equal((await store.prepare(first, old)).generation, 2);
  await store.prepare(second, old);
  assert.equal(store.device(old).status, 'active'); // preparing is not confirmation
  await store.activate(first);
  assert.equal(store.device(old).status, 'revoked');
  assert.equal(store.device(second).status, 'revoked');
  await assert.rejects(store.activate(second));
  await assert.rejects(store.prepare(randomHex(16), old));
  assert.equal(store.device(first).status, 'active');
});

test('unknown schema, truncation and tampering never silently replace bridge trust', async (t) => {
  const { directory, store } = await fixture(t);
  const path = join(directory, 'pairing.json');
  const original = await readFile(path, 'utf8');
  await store.close();
  for (const broken of ['{', JSON.stringify({ ...JSON.parse(original), version: 2 }), original.replace('sha256', 'lost-checksum')]) {
    await writeFile(path, broken, { mode: 0o600 });
    await assert.rejects(PairingStore.open(directory));
    assert.equal(await readFile(path, 'utf8'), broken);
  }
  await writeFile(path, original);
  const recovered = await PairingStore.open(directory);
  assert.equal(recovered.bridgeId, store.bridgeId);
  await recovered.close();
});

test('concurrent writers are excluded without blocking a later normal restart', async (t) => {
  const { directory, store } = await fixture(t);
  await assert.rejects(PairingStore.open(directory), /locked/);
  await store.close();
  const restarted = await PairingStore.open(directory);
  await restarted.close();
});

test('kernel lock is released automatically after a bridge process is killed', async (t) => {
  const directory = await mkdtemp(join(tmpdir(), 'attention-crash-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const module = new URL('../src/pairing-store.mjs', import.meta.url).href;
  const child = spawn(process.execPath, ['--input-type=module', '-e',
    `import { PairingStore } from ${JSON.stringify(module)}; await PairingStore.open(${JSON.stringify(directory)}); console.log('READY');`],
  { stdio: ['ignore', 'pipe', 'pipe'] });
  t.after(() => child.kill('SIGKILL'));
  assert.match((await once(child.stdout, 'data'))[0].toString(), /READY/);
  const exited = once(child, 'exit'); child.kill('SIGKILL'); await exited;
  const restarted = await PairingStore.open(directory);
  await restarted.close();
});
