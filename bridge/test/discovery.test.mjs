import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { advertisement, advertise } from '../src/discovery.mjs';

const id = '1234567890abcdef1234567890abcdef';
test('Bonjour advertises identity and protocol but no secret, token or fixed IP', () => {
  assert.deepEqual(advertisement(id, 5182), [`attention-${id}`, '_codex-attention._tcp', 'local.', '5182', `id=${id}`, 'v=1', 'tls=1']);
  for (const invalid of [0, 65536, 5182.5]) assert.throws(() => advertisement(id, invalid));
  assert.throws(() => advertisement(`${id};evil`, 5182));
});
test('native discovery uses argument arrays, can stop cleanly, and does not expose child output', () => {
  const child = new EventEmitter(); let stopped = false;
  child.kill = () => { stopped = true; };
  const stop = advertise(id, 5182, { platform: 'darwin', spawnProcess(binary, args, options) {
    assert.equal(binary, '/usr/bin/dns-sd'); assert.deepEqual(args, ['-R', ...advertisement(id, 5182)]);
    assert.deepEqual(options, { stdio: 'ignore' }); return child;
  } });
  stop(); child.emit('exit', 0); assert(stopped);
});
