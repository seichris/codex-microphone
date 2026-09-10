import { timingSafeEqual } from 'node:crypto';
import { createServer } from 'node:http';
import { createServer as createSecureServer } from 'node:https';
import { authError, exactObject, validDeviceId } from './device-auth.mjs';
import { DASHBOARD_HTML } from './dashboard.mjs';

function tokenMatches(provided, expected) {
  if (!expected) return true;
  const left = Buffer.from(provided ?? '', 'utf8');
  const right = Buffer.from(expected, 'utf8');
  return left.length === right.length && timingSafeEqual(left, right);
}

function isAuthorized(request, token) {
  if (!token) return true;
  const header = request.headers.authorization ?? '';
  const provided = header.startsWith('Bearer ') ? header.slice(7) : '';
  return tokenMatches(provided, token);
}

function sendJson(response, statusCode, body) {
  const payload = `${JSON.stringify(body)}\n`;
  response.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(payload),
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
  });
  response.end(payload);
}

const JSON_BODY_LIMIT = 4096;

async function readJsonBody(request) {
  const contentType = String(request.headers['content-type'] ?? '').toLowerCase();
  if (!contentType.startsWith('application/json')) {
    const error = new Error('Content-Type must be application/json.');
    error.code = 'unsupported_media_type';
    error.statusCode = 415;
    throw error;
  }
  const declared = Number.parseInt(String(request.headers['content-length'] ?? '0'), 10);
  if (Number.isFinite(declared) && declared > JSON_BODY_LIMIT) {
    const error = new Error('Request body is too large.');
    error.code = 'request_too_large';
    error.statusCode = 413;
    throw error;
  }

  const chunks = [];
  let length = 0;
  for await (const chunk of request) {
    length += chunk.length;
    if (length > JSON_BODY_LIMIT) {
      const error = new Error('Request body is too large.');
      error.code = 'request_too_large';
      error.statusCode = 413;
      throw error;
    }
    chunks.push(chunk);
  }
  try {
    const value = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error();
    return value;
  } catch {
    const error = new Error('Request body must be a JSON object.');
    error.code = 'invalid_json';
    error.statusCode = 400;
    throw error;
  }
}

function exactKeys(value, allowed) {
  const keys = Object.keys(value).sort();
  return keys.length === allowed.length && keys.every((key, index) => key === [...allowed].sort()[index]);
}

function validId(value, maxLength = 160) {
  return typeof value === 'string'
    && value.length >= 1
    && value.length <= maxLength
    && /^[A-Za-z0-9._:-]+$/.test(value);
}

function invalidRequest(message) {
  const error = new Error(message);
  error.code = 'invalid_request';
  error.statusCode = 400;
  return error;
}

function latestThreadId(pathname) {
  const match = pathname.match(/^\/api\/v1\/threads\/([^/]+)\/latest$/);
  if (!match) return null;
  try {
    return decodeURIComponent(match[1]);
  } catch {
    return null;
  }
}

export function createBridgeServer({ service, token, authority, device = false, tls, devicePort = 5182,
  fallbackHost = '', logger = console }) {
  if (device && (!authority || !tls)) throw new Error('Device listener requires paired TLS');
  const handler = (request, response) => {
    void (async () => {
      const url = new URL(request.url ?? '/', 'http://bridge.local');
      if (!device) {
        const remote = request.socket.remoteAddress;
        const host = new URL(`http://${request.headers.host ?? ''}`).hostname;
        if (!['127.0.0.1', '::1', '::ffff:127.0.0.1'].includes(remote)
            || !['127.0.0.1', 'localhost', '[::1]'].includes(host)
            || (request.headers.origin && request.headers.origin !== `http://${request.headers.host}`)) {
          throw authError('local_admin_only', 403);
        }
      }
      if (url.search) throw authError('query_parameters_not_supported', 400);
      if (device && request.method === 'POST' && url.pathname.startsWith('/api/v1/device/')) {
        const body = await readJsonBody(request);
        const peer = request.socket.remoteAddress ?? 'unknown';
        let result;
        if (url.pathname === '/api/v1/device/challenge') result = authority.challenge(body, peer);
        else if (url.pathname === '/api/v1/device/token') result = await authority.issue(body, peer);
        else if (url.pathname === '/api/v1/device/revoke') result = await authority.revoke(body, peer);
        else throw authError('not_found', 404);
        sendJson(response, 200, result);
        return;
      }

      if (!device && request.method === 'GET' && url.pathname === '/') {
        response.writeHead(200, {
          'Content-Type': 'text/html; charset=utf-8',
          'Cache-Control': 'no-store',
          'X-Content-Type-Options': 'nosniff',
          'Content-Security-Policy': "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'",
        });
        response.end(DASHBOARD_HTML);
        return;
      }

      if (request.method === 'GET' && url.pathname === '/healthz') {
        sendJson(response, 200, device ? { ok: true, version: 1 } : {
          ok: true,
          appServerConnected: service.connected,
          generatedAt: service.snapshot.generatedAt,
        });
        return;
      }

      if (device) authority.authorize(request, url.pathname);
      else if (!isAuthorized(request, token)) {
        response.setHeader('WWW-Authenticate', 'Bearer realm="Codex ESP32 Display"');
        sendJson(response, 401, { error: 'unauthorized' });
        return;
      }

      if (!device && authority && url.pathname.startsWith('/api/v1/admin/devices')) {
        if (request.method === 'GET' && url.pathname === '/api/v1/admin/devices') {
          sendJson(response, 200, { version: 1, bridgeId: authority.store.bridgeId, devices: authority.store.list() });
          return;
        }
        if (request.method !== 'POST') throw authError('not_found', 404);
        const body = await readJsonBody(request);
        if (url.pathname === '/api/v1/admin/devices/prepare'
            && exactObject(body, ['deviceId', 'replaces'])) {
          const record = await authority.store.prepare(body.deviceId, body.replaces);
          sendJson(response, 200, { ...record, port: devicePort, fallbackHost, provisionedAt: Math.floor(Date.now() / 1000) });
          return;
        }
        if (url.pathname === '/api/v1/admin/devices/revoke'
            && exactObject(body, ['deviceId', 'nonce'])) {
          sendJson(response, 200, await authority.store.resetAcknowledgement(body.deviceId, body.nonce));
          return;
        }
        if (url.pathname === '/api/v1/admin/devices/activate'
            && exactObject(body, ['deviceId']) && validDeviceId(body.deviceId)) {
          await authority.store.activate(body.deviceId);
          sendJson(response, 200, { ok: true });
          return;
        }
        throw authError('invalid_request', 400);
      }

      if (!device && request.method === 'GET' && url.pathname === '/api/v1/admin/task-diagnostics') {
        sendJson(response, 200, service.taskDiagnostics());
        return;
      }

      if (request.method === 'GET' && url.pathname === '/api/v1/attention') {
        sendJson(response, 200, service.snapshot);
        return;
      }

      if (request.method === 'GET' && url.pathname === '/api/v1/desktop/state') {
        sendJson(response, 200, await service.desktopState());
        return;
      }

      if (request.method === 'POST' && url.pathname === '/api/v1/desktop/focus') {
        const body = await readJsonBody(request);
        if (!exactKeys(body, ['requestId', 'threadId'])
            || !validId(body.threadId, 160)
            || !validId(body.requestId, 96)) {
          throw invalidRequest('Expected only valid threadId and requestId fields.');
        }
        sendJson(response, 200, await service.focusDesktop(body));
        return;
      }

      if (request.method === 'POST' && url.pathname === '/api/v1/desktop/voice') {
        const body = await readJsonBody(request);
        if (!exactKeys(body, ['command', 'requestId', 'threadId'])
            || !validId(body.threadId, 160)
            || !validId(body.requestId, 96)
            || !['start-or-resume', 'mute'].includes(body.command)) {
          throw invalidRequest('Expected valid threadId, requestId, and voice command fields.');
        }
        sendJson(response, 200, await service.voiceDesktop(body));
        return;
      }

      const threadId = request.method === 'GET' ? latestThreadId(url.pathname) : null;
      if (threadId) {
        try {
          sendJson(response, 200, await service.latestThread(threadId));
        } catch (error) {
          if (error?.code === 'attention_thread_not_found') {
            sendJson(response, 404, { error: 'attention_thread_not_found' });
            return;
          }
          throw error;
        }
        return;
      }

      if (request.method === 'POST' && url.pathname === '/api/v1/refresh') {
        sendJson(response, 200, await service.refresh());
        return;
      }

      sendJson(response, 404, { error: 'not_found' });
    })().catch((error) => {
      if ((error?.statusCode ?? 500) >= 500) logger.error('Bridge request failed');
      if (!response.headersSent) {
        sendJson(response, error?.statusCode ?? 500, {
          error: error?.code ?? 'internal_error',
          ...((error?.statusCode ?? 500) < 500 && error?.message ? { message: error.message } : {}),
        });
      }
      else response.destroy(error);
    });
  };
  const server = device ? createSecureServer(tls, handler) : createServer(handler);
  server.requestTimeout = 10_000;
  server.headersTimeout = 5_000;
  server.keepAliveTimeout = 2_000;
  return server;
}

export async function listen(server, { host, port }) {
  await new Promise((resolvePromise, rejectPromise) => {
    server.once('error', rejectPromise);
    server.listen(port, host, () => {
      server.off('error', rejectPromise);
      resolvePromise();
    });
  });
}
