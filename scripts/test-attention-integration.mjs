// Runs production firmware pairing/client logic against the real Node HTTPS
// bridge. Only NVS, clocks, mDNS and the ESP HTTP transport are host adapters;
// TLS is real libcurl/OpenSSL, not a permissive mocked certificate callback.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { PairingStore } from '../bridge/src/pairing-store.mjs';
import { DeviceAuthority, randomHex } from '../bridge/src/device-auth.mjs';
import { createBridgeServer, listen } from '../bridge/src/http-server.mjs';

const binary = process.argv[2];
if (!binary) throw new Error('Pass the compiled host firmware test binary');

function record(bundle, port, fallback = '') {
  const output = Buffer.alloc(2652);
  for (const [offset, value] of [[0, 0x31415043], [4, 1], [8, bundle.generation], [12, 1], [16, port], [20, Math.floor(Date.now() / 1000)]])
    output.writeUInt32LE(value, offset);
  for (const [offset, capacity, value] of [[24, 33, bundle.deviceId], [57, 33, bundle.bridgeId], [90, 65, bundle.secret],
    [155, 33, 'host-test-wifi'], [188, 65, 'host-test-password'], [253, 2048, bundle.certificate], [2301, 254, fallback]]) {
    assert(Buffer.byteLength(value) < capacity); output.write(value, offset, 'utf8');
  }
  return output;
}

async function run(mode) {
  const directory = await mkdtemp(join(tmpdir(), 'attention-end-to-end-'));
  const store = await PairingStore.open(join(directory, 'paired'));
  let authority, server, impostor, dropReply = false;
  let issues = 0;
  const actions = [];
  const service = { connected: true, snapshot: { version: 1, items: [], currentThread: null, desktopControlAvailable: true },
    async focusDesktop(body) { actions.push(body.requestId); return { version: 1, ...body, focusConfidence: 'confirmed', voiceState: 'muted' }; },
    async voiceDesktop(body) { actions.push(body.requestId); return { version: 1, ...body, focusConfidence: 'confirmed', voiceState: 'muted' }; } };
  const newAuthority = () => {
    authority = new DeviceAuthority(store);
    const issue = authority.issue.bind(authority);
    authority.issue = async (...args) => { const result = await issue(...args); ++issues; return result; };
  };
  const start = async (host = '127.0.0.1', port = 0, tls = store.tlsOptions()) => {
    server = createBridgeServer({ service, authority, device: true, tls, logger: { error: () => {} } });
    server.on('request', (request, response) => {
      if (dropReply && request.url === '/api/v1/desktop/focus') {
        dropReply = false;
        // Destroy the response only AFTER the handler executes the command.
        response.end = () => { response.destroy(); return response; };
      }
    });
    await listen(server, { host, port });
    return server.address().port;
  };
  const stop = async () => { if (server?.listening) await new Promise((resolve) => server.close(resolve)); };
  try {
    newAuthority();
    let tls = store.tlsOptions();
    if (mode === 'wrong-certificate') { impostor = await PairingStore.open(join(directory, 'impostor')); tls = impostor.tlsOptions(); }
    const port = await start('127.0.0.1', 0, tls);
    const bundle = await store.prepare(randomHex(16));
    const file = join(directory, 'device.bin');
    await writeFile(file, record(bundle, port, mode === 'fallback' ? '127.0.0.1' : ''), { mode: 0o600 });
    const child = spawn(binary, [file, mode], { stdio: ['pipe', 'pipe', 'pipe'] });
    const timer = setTimeout(() => child.kill('SIGKILL'), 25_000);
    let errorOutput = '', output = '', failure;
    let steps = Promise.resolve();
    child.stderr.on('data', (chunk) => { errorOutput += chunk; });
    createInterface({ input: child.stdout }).on('line', (line) => {
      output += `${line}\n`;
      steps = steps.then(async () => {
        if (line === 'READY_FOR_RESTART') { await stop(); newAuthority(); await start('127.0.0.1', port); child.stdin.write('\n'); }
        if (line === 'READY_FOR_LOST_REPLY') { dropReply = true; child.stdin.write('\n'); }
        if (line === 'READY_FOR_ADDRESS_CHANGE') { await stop(); await start('127.0.0.2', port); child.stdin.write('\n'); }
        if (line === 'READY_FOR_REVOKE') { await store.revoke(bundle.deviceId); child.stdin.write('\n'); }
      }).catch((error) => { failure = error; child.kill('SIGKILL'); });
    });
    const [code, signal] = await once(child, 'exit');
    clearTimeout(timer); await steps;
    if (failure) throw failure;
    assert.equal(code, 0, `${mode}: ${signal ?? ''} ${errorOutput}`);
    assert.match(output, /PASS/);
    if (mode === 'lifecycle') {
      assert.deepEqual(actions, ['firmware-focus', 'firmware-voice', 'lost-reply']);
      assert(issues >= 4); assert.equal(store.device(bundle.deviceId).status, 'revoked');
    } else if (mode !== 'fallback') assert.equal(issues, 0);
    console.log(output.trim().split('\n').at(-1));
  } finally {
    await stop(); await store.close(); await impostor?.close();
    await rm(directory, { recursive: true, force: true });
  }
}

for (const mode of ['lifecycle', 'fallback', 'unknown-service', 'protocol-mismatch', 'wrong-certificate']) {
  test(`production firmware / real Node HTTPS: ${mode}`, async () => run(mode));
}
