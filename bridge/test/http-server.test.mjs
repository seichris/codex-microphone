import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createBridgeServer } from '../src/http-server.mjs';

const THREAD_ID = '019a4444-4444-7444-8444-444444444444';

async function withServer(callback) {
  const service = {
    connected: true,
    snapshot: {
      version: 1,
      generatedAt: 'now',
      count: 1,
      totalCount: 1,
      items: [{ id: THREAD_ID }],
      diagnostics: {},
    },
    async refresh() { return this.snapshot; },
    taskDiagnostics() { return { selection: null, tasks: [{ id: THREAD_ID, title: 'Task', status: 'running' }] }; },
    async desktopState() {
      return {
        version: 1,
        threadId: THREAD_ID,
        focusConfidence: 'inferred',
        voiceState: 'muted',
      };
    },
    async focusDesktop(body) {
      return {
        version: 1,
        requestId: body.requestId,
        threadId: body.threadId,
        focusConfidence: 'inferred',
        voiceState: 'muted',
      };
    },
    async voiceDesktop(body) {
      return {
        version: 1,
        requestId: body.requestId,
        threadId: body.threadId,
        focusConfidence: 'inferred',
        voiceState: body.command === 'mute' ? 'muted' : 'listening',
      };
    },
    async latestThread(threadId) {
      if (threadId !== THREAD_ID) {
        const error = new Error('missing');
        error.code = 'attention_thread_not_found';
        throw error;
      }
      return { version: 1, id: threadId, kind: 'agent', text: 'Latest result' };
    },
  };
  const server = createBridgeServer({ service, token: 'abcdefghijklmnopqrstuvwxyz123456' });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { port } = server.address();
  try { await callback(`http://127.0.0.1:${port}`); }
  finally { await new Promise((resolve) => server.close(resolve)); }
}

test('health is public while attention data requires bearer token', async () => {
  await withServer(async (base) => {
    assert.equal((await fetch(`${base}/healthz`)).status, 200);
    assert.equal((await fetch(`${base}/api/v1/attention`)).status, 401);
    const response = await fetch(`${base}/api/v1/attention`, {
      headers: { Authorization: 'Bearer abcdefghijklmnopqrstuvwxyz123456' },
    });
    assert.equal(response.status, 200);
    assert.equal((await response.json()).version, 1);
  });
});

test('task diagnostics require local admin authentication', async () => {
  await withServer(async base => {
    assert.equal((await fetch(`${base}/api/v1/admin/task-diagnostics`)).status, 401);
    const response = await fetch(`${base}/api/v1/admin/task-diagnostics`, {
      headers: { Authorization: 'Bearer abcdefghijklmnopqrstuvwxyz123456' },
    });
    assert.equal(response.status, 200);
    assert.deepEqual(await response.json(), { selection: null, tasks: [{ id: THREAD_ID, title: 'Task', status: 'running' }] });
  });
});

test('latest text endpoint is authenticated and rejects non-attention threads', async () => {
  await withServer(async (base) => {
    const endpoint = `${base}/api/v1/threads/${THREAD_ID}/latest`;
    assert.equal((await fetch(endpoint)).status, 401);

    const headers = { Authorization: 'Bearer abcdefghijklmnopqrstuvwxyz123456' };
    const response = await fetch(endpoint, { headers });
    assert.equal(response.status, 200);
    assert.deepEqual(await response.json(), {
      version: 1,
      id: THREAD_ID,
      kind: 'agent',
      text: 'Latest result',
    });

    const missing = await fetch(`${base}/api/v1/threads/not-in-inbox/latest`, { headers });
    assert.equal(missing.status, 404);
  });
});

test('Desktop endpoints validate JSON strictly and route authenticated commands', async () => {
  await withServer(async (base) => {
    const headers = {
      Authorization: 'Bearer abcdefghijklmnopqrstuvwxyz123456',
      'Content-Type': 'application/json',
    };
    const state = await fetch(`${base}/api/v1/desktop/state`, { headers });
    assert.equal(state.status, 200);
    assert.equal((await state.json()).threadId, THREAD_ID);

    const focus = await fetch(`${base}/api/v1/desktop/focus`, {
      method: 'POST',
      headers,
      body: JSON.stringify({ threadId: THREAD_ID, requestId: 'focus-1' }),
    });
    assert.equal(focus.status, 200);
    assert.equal((await focus.json()).requestId, 'focus-1');

    const voice = await fetch(`${base}/api/v1/desktop/voice`, {
      method: 'POST',
      headers,
      body: JSON.stringify({
        threadId: THREAD_ID,
        requestId: 'voice-1',
        command: 'start-or-resume',
      }),
    });
    assert.equal(voice.status, 200);
    assert.equal((await voice.json()).voiceState, 'listening');

    const unknownField = await fetch(`${base}/api/v1/desktop/focus`, {
      method: 'POST',
      headers,
      body: JSON.stringify({ threadId: THREAD_ID, requestId: 'focus-2', extra: true }),
    });
    assert.equal(unknownField.status, 400);

    const wrongType = await fetch(`${base}/api/v1/desktop/voice`, {
      method: 'POST',
      headers: { Authorization: headers.Authorization, 'Content-Type': 'text/plain' },
      body: '{}',
    });
    assert.equal(wrongType.status, 415);
  });
});
