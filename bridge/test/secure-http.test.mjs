import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import https from 'node:https';
import http from 'node:http';
import { createConnection } from 'node:net';
import { once } from 'node:events';
import { PairingStore } from '../src/pairing-store.mjs';
import { DeviceAuthority, proof, randomHex } from '../src/device-auth.mjs';
import { createBridgeServer, listen } from '../src/http-server.mjs';

async function fixture(t) {
  const directory = await mkdtemp(join(tmpdir(), 'attention-tls-'));
  const store = await PairingStore.open(directory);
  const authority = new DeviceAuthority(store);
  const calls = [];
  const service = { connected: true, snapshot: { version: 1, items: [], currentThread: null },
    async refresh() { calls.push('refresh'); return {}; },
    async latestThread(id) { calls.push('detail'); return { version: 1, id }; },
    async desktopState() { calls.push('state'); return { version: 1, threadId: null }; },
    async focusDesktop(body) { calls.push('focus'); return { version: 1, ...body }; },
    async voiceDesktop(body) { calls.push('voice'); return { version: 1, ...body }; } };
  const logs = [], adminToken = randomHex();
  const logger = { error: (entry) => logs.push(entry) };
  const device = createBridgeServer({ service, authority, device: true, tls: store.tlsOptions(), logger });
  const admin = createBridgeServer({ service, authority, token: adminToken, logger });
  await listen(device, { host: '127.0.0.1', port: 0 });
  await listen(admin, { host: '127.0.0.1', port: 0 });
  t.after(async () => {
    await Promise.all([device, admin].map((server) => new Promise((resolve) => server.close(resolve))));
    await store.close(); await rm(directory, { recursive: true, force: true });
  });
  const request = (path, { body, headers = {}, ca = store.certificate,
    servername = `attention-${store.bridgeId}.local`, method = body ? 'POST' : 'GET' } = {}) => new Promise((resolve, reject) => {
    const json = body ? JSON.stringify(body) : undefined;
    const req = https.request({ hostname: '127.0.0.1', port: device.address().port, path,
      method, ca, servername, rejectUnauthorized: true, agent: false,
      headers: { ...(json ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(json) } : {}), ...headers } }, (res) => {
      let data = ''; res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => resolve({ status: res.statusCode, body: JSON.parse(data) }));
    });
    req.setTimeout(3000, () => req.destroy(new Error('test request timed out')));
    req.on('error', reject); req.end(json);
  });
  const paired = await store.prepare(randomHex(16));
  const getToken = async () => {
    const challenge = (await request('/api/v1/device/challenge', { body: { deviceId: paired.deviceId, generation: paired.generation } })).body;
    return (await request('/api/v1/device/token', { body: { deviceId: paired.deviceId, generation: paired.generation,
      sessionId: challenge.sessionId, challenge: challenge.challenge,
      proof: proof(paired.secret, 'auth', paired.bridgeId, paired.deviceId, paired.generation, challenge.sessionId, challenge.challenge) } })).body;
  };
  let sequence = 0;
  const headers = (token) => ({ Authorization: `Bearer ${token.accessToken}`, 'X-Codex-Device': paired.deviceId,
    'X-Codex-Generation': String(paired.generation), 'X-Codex-Session': token.sessionId, 'X-Codex-Sequence': String(++sequence) });
  return { store, authority, calls, logs, device, admin, adminToken, request, paired, getToken, headers };
}

test('actual TLS listener verifies paired identity independently of its IP and rejects wrong trust/name', async (t) => {
  const f = await fixture(t);
  assert.deepEqual((await f.request('/healthz')).body, { ok: true, version: 1 });
  await assert.rejects(f.request('/healthz', { servername: 'unpaired.local' }), { code: 'ERR_TLS_CERT_ALTNAME_INVALID' });
  // Explicit null bypasses the fixture default and uses the OS trust set.
  await assert.rejects(f.request('/healthz', { ca: null }));
  const socket = createConnection(f.device.address().port, '127.0.0.1');
  const closed = once(socket, 'close'); socket.on('error', () => {});
  socket.write('GET /api/v1/attention HTTP/1.1\r\nHost: localhost\r\n\r\n');
  await closed;
  assert.equal(f.calls.length, 0);
});

test('TLS attention/list/detail/focus/voice work only after HMAC authentication with exact device scope', async (t) => {
  const f = await fixture(t);
  assert.equal((await f.request('/api/v1/attention')).status, 401);
  assert.equal((await f.request('/api/v1/attention', { headers: { Authorization: `Bearer ${f.adminToken}` } })).status, 401);
  const token = await f.getToken();
  const h = f.headers(token);
  assert.equal((await f.request('/api/v1/attention', { headers: h })).status, 200);
  assert.equal((await f.request('/api/v1/attention', { headers: h })).status, 401);
  assert.equal((await f.request('/api/v1/desktop/state', { headers: f.headers(token) })).status, 200);
  assert.equal((await f.request('/api/v1/threads/019a-4/latest', { headers: f.headers(token) })).status, 200);
  assert.equal((await f.request('/api/v1/desktop/focus', { headers: f.headers(token), body: { threadId: '019a-4', requestId: 'focus-1' } })).status, 200);
  assert.equal((await f.request('/api/v1/desktop/voice', { headers: f.headers(token), body: { threadId: '019a-4', requestId: 'voice-1', command: 'mute' } })).status, 200);
  for (const path of ['/api/v1/refresh', '/api/v1/admin/devices/prepare'])
    assert.equal((await f.request(path, { headers: f.headers(token), body: {} })).status, 403);
  assert.deepEqual(f.calls, ['state', 'detail', 'focus', 'voice']);
});

test('local admin excludes device bearer, cross-origin requests and DNS-rebinding hosts', async (t) => {
  const f = await fixture(t), token = await f.getToken();
  const url = `http://127.0.0.1:${f.admin.address().port}/api/v1/admin/devices`;
  assert.equal((await fetch(url, { headers: { Authorization: `Bearer ${token.accessToken}` } })).status, 401);
  assert.equal((await fetch(url, { headers: { Authorization: `Bearer ${f.adminToken}`, Origin: 'https://attacker.example' } })).status, 403);
  // Fetch normalizes Host; use a raw HTTP request to exercise rebinding.
  const rebindingStatus = await new Promise((resolve, reject) => {
    const req = http.get(url, { headers: { Authorization: `Bearer ${f.adminToken}`, Host: 'attacker.example' } }, (res) => {
      res.resume(); res.on('end', () => resolve(res.statusCode));
    });
    req.on('error', reject);
  });
  assert.equal(rebindingStatus, 403);
  const result = await (await fetch(url, { headers: { Authorization: `Bearer ${f.adminToken}` } })).json();
  assert(!JSON.stringify(result).includes(f.paired.secret));
  assert(!JSON.stringify(result).includes(token.accessToken));
  assert(!JSON.stringify(f.logs).includes(f.paired.secret));
});

test('invalid and oversized commands never reach desktop actions; query credentials are rejected', async (t) => {
  const f = await fixture(t), token = await f.getToken();
  assert.equal((await f.request('/api/v1/desktop/focus', { headers: f.headers(token), body: { threadId: '019a-4', requestId: 'id', extra: true } })).status, 400);
  assert.equal((await f.request('/api/v1/desktop/focus', { headers: f.headers(token), body: { text: 'x'.repeat(5000) } })).status, 413);
  assert.equal((await f.request(`/api/v1/attention?token=${token.accessToken}`, { headers: f.headers(token) })).status, 400);
  assert.equal(f.calls.length, 0);
});
