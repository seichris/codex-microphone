import { createHmac, randomBytes, timingSafeEqual } from 'node:crypto';

export const PROTOCOL_VERSION = 1;
export const DEVICE_SERVICE = '_codex-attention._tcp';
export const ACCESS_TTL_SECONDS = 300;
const HEX_ID = /^[a-f0-9]{32}$/;
const HEX_SECRET = /^[a-f0-9]{64}$/;
export const validDeviceId = (value) => typeof value === 'string' && HEX_ID.test(value);
export const validSecret = (value) => typeof value === 'string' && HEX_SECRET.test(value);
export const validGeneration = (value) => Number.isSafeInteger(value) && value > 0 && value <= 0xffffffff;
export const randomHex = (bytes = 32) => randomBytes(bytes).toString('hex');

export function authError(code = 'unauthorized', statusCode = 401) {
  return Object.assign(new Error(code), { code, statusCode });
}

export function constantTimeEqual(provided, expected) {
  if (typeof provided !== 'string' || typeof expected !== 'string') return false;
  const a = Buffer.from(provided);
  const b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
}

export function proof(secret, domain, bridgeId, deviceId, generation, ...parts) {
  if (!validSecret(secret) || !validDeviceId(bridgeId) || !validDeviceId(deviceId)
      || !validGeneration(generation) || !parts.every(validSecret)) throw authError('invalid_request', 400);
  return createHmac('sha256', Buffer.from(secret, 'hex'))
    .update([`codex-attention-${domain}-v1`, bridgeId, deviceId, String(generation), ...parts].join('\n'))
    .digest('hex');
}

export function exactObject(value, keys) {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    && Object.keys(value).length === keys.length && keys.every((key) => Object.hasOwn(value, key));
}

export function deviceRoute(method, path) {
  return (method === 'GET' && (path === '/api/v1/attention' || path === '/api/v1/desktop/state'
      || /^\/api\/v1\/threads\/[A-Za-z0-9._:-]{1,160}\/latest$/.test(path)))
    || (method === 'POST' && ['/api/v1/desktop/focus', '/api/v1/desktop/voice'].includes(path));
}

// A fresh authority is a fresh server session: no access token or challenge
// survives restart. Durable credentials are consulted on EVERY authorization.
export class DeviceAuthority {
  constructor(store, { now = () => performance.now() } = {}) {
    this.store = store;
    this.now = now;
    this.sessionId = randomHex();
    this.challenges = new Map();
    this.tokens = new Map();
    this.rates = new Map();
  }

  rateLimit(peer) {
    const now = this.now();
    for (const [key, entry] of this.rates) if (entry.until <= now) this.rates.delete(key);
    const entry = this.rates.get(peer) ?? { until: now + 60_000, count: 0 };
    if (entry.count >= 120 || (!this.rates.has(peer) && this.rates.size >= 128)) {
      throw authError('rate_limited', 429);
    }
    entry.count += 1;
    this.rates.set(peer, entry);
  }

  record(deviceId, generation, allowRevoked = false) {
    if (!validDeviceId(deviceId) || !validGeneration(generation)) throw authError('invalid_request', 400);
    const record = this.store.device(deviceId);
    if (!record || record.generation !== generation || (!allowRevoked && record.status === 'revoked')) {
      throw authError('device_revoked_or_unknown', 403);
    }
    return record;
  }

  challenge(body, peer = 'local') {
    this.rateLimit(peer);
    if (!exactObject(body, ['deviceId', 'generation'])) throw authError('invalid_request', 400);
    this.record(body.deviceId, body.generation);
    const challenge = randomHex();
    this.challenges.set(body.deviceId, { challenge, generation: body.generation, until: this.now() + 30_000 });
    return { version: 1, bridgeId: this.store.bridgeId, sessionId: this.sessionId, challenge, expiresIn: 30 };
  }

  async issue(body, peer = 'local') {
    this.rateLimit(peer);
    if (!exactObject(body, ['deviceId', 'generation', 'sessionId', 'challenge', 'proof'])
        || !validSecret(body.sessionId) || !validSecret(body.challenge) || !validSecret(body.proof)) {
      throw authError('invalid_request', 400);
    }
    const record = this.record(body.deviceId, body.generation);
    const challenge = this.challenges.get(body.deviceId);
    // Consume synchronously, before persistence awaits: concurrent completions
    // cannot redeem the same challenge twice.
    this.challenges.delete(body.deviceId);
    if (body.sessionId !== this.sessionId || !challenge || challenge.until <= this.now()
        || challenge.generation !== body.generation || challenge.challenge !== body.challenge
        || !constantTimeEqual(body.proof, proof(record.secret, 'auth', this.store.bridgeId,
          body.deviceId, body.generation, this.sessionId, body.challenge))) throw authError();
    if (record.status === 'pending') await this.store.activate(body.deviceId);
    // A concurrent administrative revocation must win over an in-flight issue.
    this.record(body.deviceId, body.generation);
    this.tokens.delete(body.deviceId);
    const accessToken = randomHex();
    this.tokens.set(body.deviceId, {
      accessToken, generation: body.generation, until: this.now() + ACCESS_TTL_SECONDS * 1000, sequence: 0,
    });
    return { version: 1, bridgeId: this.store.bridgeId, sessionId: this.sessionId,
      generation: body.generation, accessToken, expiresIn: ACCESS_TTL_SECONDS };
  }

  authorize(request, path) {
    const headers = request.headers;
    const deviceId = headers['x-codex-device'];
    const generationText = headers['x-codex-generation'];
    const sequenceText = headers['x-codex-sequence'];
    if (!validDeviceId(deviceId) || !/^[1-9][0-9]{0,9}$/.test(generationText ?? '')
        || !/^[1-9][0-9]{0,9}$/.test(sequenceText ?? '')) throw authError();
    const generation = Number(generationText);
    const sequence = Number(sequenceText);
    this.record(deviceId, generation);
    const token = this.tokens.get(deviceId);
    if (!token || token.until <= this.now() || token.generation !== generation
        || headers['x-codex-session'] !== this.sessionId
        || !constantTimeEqual(headers.authorization, `Bearer ${token.accessToken}`)) throw authError();
    if (sequence > 0xffffffff || sequence <= token.sequence) throw authError('replayed_request');
    if (!deviceRoute(request.method, path)) throw authError('insufficient_scope', 403);
    token.sequence = sequence;
  }

  async revoke(body, peer = 'local') {
    this.rateLimit(peer);
    if (!exactObject(body, ['deviceId', 'generation', 'nonce', 'proof'])
        || !validSecret(body.nonce) || !validSecret(body.proof)) throw authError('invalid_request', 400);
    const record = this.record(body.deviceId, body.generation, true);
    if (!constantTimeEqual(body.proof, proof(record.secret, 'revoke', this.store.bridgeId,
      body.deviceId, body.generation, body.nonce))) throw authError();
    await this.store.revoke(body.deviceId);
    this.tokens.delete(body.deviceId);
    this.challenges.delete(body.deviceId);
    return { version: 1, deviceId: body.deviceId, nonce: body.nonce,
      proof: proof(record.secret, 'revoked', this.store.bridgeId, body.deviceId, body.generation, body.nonce) };
  }
}
