import test from 'node:test';
import assert from 'node:assert/strict';
import { chmod, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { CodexAttentionService } from '../src/service.mjs';

const WAITING = '019a4444-4444-7444-8444-444444444444';
const UNREAD = '019a5555-5555-7555-8555-555555555555';
const PINNED = '019a6666-6666-7666-8666-666666666666';
const OLD = '019a7777-7777-7777-8777-777777777777';

test('service performs App Server handshake and merges unread plus pinned state', async () => {
  const root = await mkdtemp(join(tmpdir(), 'codex-attention-'));
  const codexHome = join(root, 'codex-home');
  const fakeCodex = join(root, 'fake-codex.mjs');
  await mkdir(codexHome, { recursive: true });
  await writeFile(join(codexHome, '.codex-global-state.json'), JSON.stringify({
    'electron-persisted-atom-state': {
      'unread-thread-ids-by-host-v1': { local: [UNREAD] },
    },
  }));
  await writeFile(fakeCodex, `#!/usr/bin/env node
import readline from 'node:readline';
const rl = readline.createInterface({ input: process.stdin });
const threads = [
  { id: '${WAITING}', name: 'Approve firmware', preview: '', cwd: '/tmp/hardware', updatedAt: 1800000000, status: { type: 'active', activeFlags: ['waitingOnApproval'] } },
  { id: '${UNREAD}', name: 'Finished bridge', preview: '', cwd: '/tmp/bridge', updatedAt: 1799999990, status: { type: 'idle' } },
  { id: '${PINNED}', name: 'Pinned roadmap', preview: '', cwd: '/tmp/roadmap', updatedAt: 1799999980, status: { type: 'idle' }, section: { id: '01984de2-8f74-7c91-a3b2-5c5e937cf318', name: 'Pinned' } }
];
rl.on('line', line => {
  const message = JSON.parse(line);
  if (message.method === 'initialize') {
    process.stdout.write(JSON.stringify({ id: message.id, result: { codexHome: process.env.CODEX_HOME } }) + '\\n');
  } else if (message.method === 'thread/list') {
    process.stdout.write(JSON.stringify({ id: message.id, result: { data: threads, nextCursor: null } }) + '\\n');
  } else if (message.method === 'thread/read') {
    const thread = { id: '${OLD}', name: 'Old unpinned task', cwd: '/tmp/old-project', updatedAt: 1700000000, status: { type: 'idle' } };
    process.stdout.write(JSON.stringify({ id: message.id, result: { thread } }) + '\\n');
  } else if (message.method === 'thread/turns/list') {
    const text = message.params.threadId === '${WAITING}'
      ? 'Please approve the firmware flash.'
      : "Latest result\\n\\n<oai-mem-citation>\\n<citation_entries>hidden\\n</citation_entries>\\n</oai-mem-citation>";
    process.stdout.write(JSON.stringify({ id: message.id, result: { data: [{ id: 'turn-1', items: [{ type: 'agentMessage', id: 'item-1', text }] }], nextCursor: null } }) + '\\n');
  }
});
`);
  await chmod(fakeCodex, 0o755);

  let controllerState = {
    available: true,
    threadId: PINNED,
    focusConfidence: 'inferred',
    voiceState: 'muted',
    capabilities: { desktopFocus: true, desktopVoiceHotkey: true },
  };
  let controllerFailed = false;
  const service = new CodexAttentionService({
    codexBin: fakeCodex,
    codexHome,
    maxThreads: 100,
    maxItems: 30,
    attentionFilter: 'all',
    pollIntervalMs: 60_000,
  }, {
    logger: { error() {}, log() {} },
    desktopController: {
      async state() {
        if (controllerFailed) throw new Error('controller disconnected');
        return controllerState;
      },
    },
  });

  try {
    await service.start();
    assert.equal(service.connected, true);
    assert.deepEqual(service.snapshot.items.map((item) => item.id), [WAITING, UNREAD]);
    assert.equal(service.snapshot.items[1].unread, true);
    assert.equal(service.snapshot.desktopControlAvailable, true);
    assert.equal(service.snapshot.currentThread.id, PINNED);
    assert.equal(service.snapshot.currentThread.focusConfidence, 'inferred');
    assert.match(service.snapshot.currentThread.project, /^Device selected · /);
    assert.equal(service.snapshot.capabilities.desktopVoiceHotkey, true);
    assert.equal(service.snapshot.diagnostics.desktopStateAvailable, true);

    const detail = await service.latestThread(WAITING);
    assert.equal(detail.id, WAITING);
    assert.equal(detail.kind, 'agent');
    assert.equal(detail.text, 'Please approve the firmware flash.');

    const sanitized = await service.latestThread(PINNED);
    assert.equal(sanitized.text, 'Latest result');

    // Manual navigation changes the current card independently of the inbox.
    // An old task outside all list pages is resolved by exact ID.
    controllerState = { ...controllerState, threadId: OLD, focusConfidence: 'confirmed', voiceState: 'unknown' };
    await service.refresh();
    assert.equal(service.snapshot.currentThread.id, OLD);
    assert.equal(service.snapshot.currentThread.title, 'Old unpinned task');
    assert.equal(service.snapshot.currentThread.focusConfidence, 'confirmed');
    assert.equal(service.snapshot.items.some((item) => item.id === OLD), false);
    controllerState = { ...controllerState, threadId: UNREAD };
    await service.refresh();
    assert.equal(service.snapshot.currentThread.id, UNREAD);
    assert.equal(service.snapshot.currentThread.voiceState, 'unknown');

    // The companion can be healthy while the Mac selection is unobserved.
    // Unread/pinned/recent tasks are not evidence of the selected Mac task.
    controllerState = { ...controllerState, threadId: null, focusConfidence: 'unavailable', voiceState: 'unknown' };
    await service.refresh();
    assert.equal(service.snapshot.currentThread, null);
    assert.equal(service.snapshot.desktopControlAvailable, true);
    assert.equal(service.snapshot.diagnostics.desktopStateAvailable, true);
    assert.equal((await service.desktopState()).focusConfidence, 'unavailable');
    assert.equal(service.snapshot.items.some((item) => item.id === PINNED), true);

    controllerFailed = true;
    await service.refresh();
    assert.equal(service.snapshot.currentThread, null);
    assert.equal(service.snapshot.desktopControlAvailable, false);
    assert.equal(service.snapshot.diagnostics.desktopStateAvailable, true);
    assert.equal((await service.desktopState()).desktopControlAvailable, false);

    controllerFailed = false;
    controllerState = { ...controllerState, threadId: PINNED, focusConfidence: 'inferred' };
    await service.refresh();
    assert.equal(service.snapshot.desktopControlAvailable, true);
    assert.equal(service.snapshot.currentThread.id, PINNED);
    assert.equal(service.snapshot.currentThread.focusConfidence, 'inferred');
    assert.equal(service.snapshot.items.some((item) => item.id === PINNED), false);
  } finally {
    await service.stop();
    await rm(root, { recursive: true, force: true });
  }
});
