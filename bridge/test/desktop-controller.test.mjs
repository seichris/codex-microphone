import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  DesktopControllerClient,
  mergeDesktopState,
  normalizeDesktopState,
  unavailableDesktopState,
  validOpaqueId,
  validRequestId,
} from '../src/desktop-controller.mjs';

const THREAD_ID = '019a4444-4444-7444-8444-444444444444';

test('normalizes Desktop state and fails closed for malformed values', () => {
  assert.deepEqual(normalizeDesktopState(null), unavailableDesktopState());
  assert.deepEqual(normalizeDesktopState({
    available: true,
    threadId: THREAD_ID,
    focusConfidence: 'confirmed',
    voiceState: 'listening',
    capabilities: { desktopFocus: true, desktopVoiceHotkey: true, wirelessMicrophone: true },
  }), {
    available: true,
    threadId: THREAD_ID,
    focusConfidence: 'confirmed',
    voiceState: 'listening',
    capabilities: {
      desktopFocus: true,
      desktopVoiceHotkey: true,
      powerButtonLongPress: false,
      wirelessMicrophone: true,
    },
    wirelessSession: {
      sessionId: null,
      transport: null,
      state: 'idle',
      revision: 0,
      errorCode: null,
    },
  });
  assert.equal(normalizeDesktopState({ threadId: 'bad id', voiceState: 'listening' }).threadId, null);
});

test('validates opaque IDs without requiring UUIDs', () => {
  assert.equal(validOpaqueId(THREAD_ID), true);
  assert.equal(validOpaqueId('opaque_thread-id-123456'), true);
  assert.equal(validOpaqueId('contains spaces'), false);
  assert.equal(validRequestId('boot:42.1'), true);
  assert.equal(validRequestId('../escape'), false);
});

test('controller without private IPC reports unavailable and rejects commands', async () => {
  const controller = new DesktopControllerClient({ directory: '', token: '' });
  assert.deepEqual(await controller.state(), unavailableDesktopState());
  await assert.rejects(
    controller.focus(THREAD_ID, 'request-1'),
    { code: 'desktop_control_unavailable', statusCode: 503 },
  );
});

test('exchanges a private IPC command and reuses an idempotent response', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'desktop-controller-test-'));
  const token = 'abcdefghijklmnopqrstuvwxyz123456';
  await mkdir(join(directory, 'requests'));
  await mkdir(join(directory, 'responses'));
  let received = 0;

  const responder = (async () => {
    const deadline = Date.now() + 2000;
    while (Date.now() < deadline) {
      const files = (await readdir(join(directory, 'requests')))
        .filter((file) => file.endsWith('.json'));
      if (files.length > 0) {
        const request = JSON.parse(await readFile(join(directory, 'requests', files[0]), 'utf8'));
        received += 1;
        await writeFile(join(directory, 'responses', `${request.ipcId}.json`), JSON.stringify({
          version: 1,
          ipcId: request.ipcId,
          ok: true,
          state: {
            threadId: request.threadId,
            focusConfidence: 'inferred',
            voiceState: 'muted',
            capabilities: { desktopFocus: true },
          },
        }));
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error('Timed out waiting for IPC request.');
  })();

  try {
    const controller = new DesktopControllerClient({ directory, token, timeoutMs: 1000 });
    const first = await controller.focus(THREAD_ID, 'focus-idempotent');
    await responder;
    const second = await controller.focus(THREAD_ID, 'focus-idempotent');
    assert.deepEqual(second, first);
    assert.equal(first.threadId, THREAD_ID);
    assert.equal(received, 1);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test('a responding controller with no target does not establish Mac selection', () => {
  const state = normalizeDesktopState({
    available: true,
    threadId: null,
    focusConfidence: 'confirmed',
    capabilities: { desktopFocus: true },
  });
  assert.equal(state.available, true);
  assert.equal(state.threadId, null);
  assert.equal(state.focusConfidence, 'unavailable');
  assert.equal(state.voiceState, 'unknown');
  assert.equal(unavailableDesktopState().available, false);
});

test('normalizes an active wireless session without trusting malformed revisions', () => {
  const state = normalizeDesktopState({
    available: true,
    threadId: THREAD_ID,
    focusConfidence: 'confirmed',
    voiceState: 'listening',
    capabilities: { wirelessMicrophone: true },
    wirelessSession: {
      sessionID: 'AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE',
      transport: 'wifi',
      state: 'listening',
      revision: 9,
      errorCode: 'capture_timeout',
    },
  });
  assert.deepEqual(state.wirelessSession, {
    sessionId: 'AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE',
    transport: 'wifi',
    state: 'listening',
    revision: 9,
    errorCode: 'capture_timeout',
  });
  assert.equal(normalizeDesktopState({ wirelessSession: { revision: -1, state: 'bogus' } }).wirelessSession.revision, 0);
});

test('does not let an older wireless revision overwrite a newer session', () => {
  const newer = normalizeDesktopState({
    available: true,
    wirelessSession: { sessionID: 'new-session', transport: 'wifi', state: 'listening', revision: 8 },
  });
  const older = normalizeDesktopState({
    available: true,
    wirelessSession: { sessionID: 'old-session', transport: 'wifi', state: 'idle', revision: 7 },
  });
  const merged = mergeDesktopState(newer, older);
  assert.deepEqual(merged.wirelessSession, newer.wirelessSession);
  assert.equal(merged.available, true);
});

test('an unavailable companion clears the last wireless session summary', () => {
  const active = normalizeDesktopState({
    available: true,
    wirelessSession: { sessionID: 'active-session', transport: 'wifi', state: 'listening', revision: 12 },
  });
  const merged = mergeDesktopState(active, unavailableDesktopState());
  assert.equal(merged.available, false);
  assert.deepEqual(merged.wirelessSession, unavailableDesktopState().wirelessSession);
});
