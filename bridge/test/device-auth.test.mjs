import test from 'node:test';
import assert from 'node:assert/strict';
import { DeviceAuthority, proof, randomHex, deviceRoute } from '../src/device-auth.mjs';

function fixture() {
  const id = randomHex(16), bridgeId = randomHex(16), secret = randomHex();
  const record = { generation: 1, secret, status: 'active' };
  const store = { bridgeId, device: (key) => key === id ? { ...record } : null,
    async activate() { record.status = 'active'; }, async revoke() { record.status = 'revoked'; } };
  let now = 1000;
  const authority = new DeviceAuthority(store, { now: () => now });
  const challengeBody = () => ({ deviceId: id, generation: 1 });
  const redemption = (challenge) => ({ ...challengeBody(), sessionId: challenge.sessionId, challenge: challenge.challenge,
    proof: proof(secret, 'auth', bridgeId, id, 1, challenge.sessionId, challenge.challenge) });
  const issue = async (auth = authority) => auth.issue(redemption(auth.challenge(challengeBody())));
  const request = (token, sequence = 1, method = 'GET') => ({ method, headers: { authorization: `Bearer ${token.accessToken}`,
    'x-codex-device': id, 'x-codex-generation': '1', 'x-codex-session': token.sessionId, 'x-codex-sequence': String(sequence) } });
  return { id, bridgeId, secret, record, store, authority, issue, redemption, challengeBody, request, setNow: (value) => { now = value; } };
}

const unauthorized = (error) => error.statusCode === 401;

test('one-use challenges cannot be redeemed concurrently or after their deadline', async () => {
  const f = fixture();
  const body = f.redemption(f.authority.challenge(f.challengeBody()));
  const outcomes = await Promise.allSettled([f.authority.issue(body), f.authority.issue(body)]);
  assert.equal(outcomes.filter((item) => item.status === 'fulfilled').length, 1);
  const expired = f.redemption(f.authority.challenge(f.challengeBody()));
  f.setNow(31_000);
  await assert.rejects(f.authority.issue(expired), unauthorized);
});

test('wrong proof consumes the challenge; wrong ID, generation and session fail closed', async () => {
  const f = fixture();
  const body = f.redemption(f.authority.challenge(f.challengeBody()));
  await assert.rejects(f.authority.issue({ ...body, proof: randomHex() }), unauthorized);
  await assert.rejects(f.authority.issue(body), unauthorized);
  for (const patch of [{ deviceId: randomHex(16) }, { generation: 2 }, { generation: 0 }, { generation: 0x100000000 }]) {
    assert.throws(() => f.authority.challenge({ ...f.challengeBody(), ...patch }));
  }
  const next = f.redemption(f.authority.challenge(f.challengeBody()));
  await assert.rejects(f.authority.issue({ ...next, sessionId: randomHex() }), unauthorized);
});

test('every request binds token, identity, generation, session and increasing sequence', async () => {
  const f = fixture();
  const token = await f.issue();
  const request = f.request(token);
  f.authority.authorize(request, '/api/v1/attention');
  assert.throws(() => f.authority.authorize(request, '/api/v1/attention'), unauthorized);
  f.authority.authorize(f.request(token, 2), '/api/v1/desktop/state');
  for (const patch of [
    { 'x-codex-device': randomHex(16) }, { 'x-codex-generation': '2' }, { 'x-codex-generation': '01' },
    { 'x-codex-session': randomHex() }, { 'x-codex-sequence': '4294967296' },
    { 'x-codex-sequence': '3.0' }, { authorization: `Bearer ${f.secret}` },
  ]) assert.throws(() => f.authority.authorize({ ...request, headers: { ...f.request(token, 3).headers, ...patch } }, '/api/v1/attention'));
  f.authority.authorize(f.request(token, 3), '/api/v1/attention');
});

test('expiry, credential generation change, revocation and bridge restart invalidate old access', async () => {
  const f = fixture();
  const token = await f.issue();
  f.setNow(301_000);
  assert.throws(() => f.authority.authorize(f.request(token), '/api/v1/attention'), unauthorized);
  const rotated = await f.issue();
  assert.throws(() => f.authority.authorize(f.request(token), '/api/v1/attention'), unauthorized);
  const restarted = new DeviceAuthority(f.store);
  assert.throws(() => restarted.authorize(f.request(rotated), '/api/v1/attention'), unauthorized);
  f.record.generation = 2;
  assert.throws(() => f.authority.authorize(f.request(rotated), '/api/v1/attention'));
  f.record.generation = 1;
  f.record.status = 'revoked';
  assert.throws(() => f.authority.authorize(f.request(rotated), '/api/v1/attention'));
  await assert.rejects(f.issue());
});

test('token rotation does not preserve even an unused predecessor token', async () => {
  const f = fixture();
  const old = await f.issue(), current = await f.issue();
  assert.throws(() => f.authority.authorize(f.request(old), '/api/v1/attention'));
  f.authority.authorize(f.request(current), '/api/v1/attention');
});

test('device scope cannot refresh the server, administer pairing, or reach other APIs', async () => {
  const f = fixture(), token = await f.issue();
  for (const path of ['/api/v1/refresh', '/api/v1/admin/devices', '/api/v1/admin/devices/prepare', '/', '/api/v1/thread/delete']) {
    assert.throws(() => f.authority.authorize(f.request(token, 1, 'POST'), path), (error) => error.statusCode === 403);
  }
  assert(deviceRoute('GET', '/api/v1/threads/019a-4/latest'));
  assert(!deviceRoute('GET', '/api/v1/threads/../../latest'));
  f.authority.authorize(f.request(token, 1, 'POST'), '/api/v1/desktop/voice');
});

test('reset acknowledgement is authentic, idempotent and cannot revive a revoked credential', async () => {
  const f = fixture(), token = await f.issue(), nonce = randomHex();
  const body = { ...f.challengeBody(), nonce, proof: proof(f.secret, 'revoke', f.bridgeId, f.id, 1, nonce) };
  await assert.rejects(f.authority.revoke({ ...body, proof: randomHex() }));
  const expected = proof(f.secret, 'revoked', f.bridgeId, f.id, 1, nonce);
  assert.equal((await f.authority.revoke(body)).proof, expected);
  assert.equal((await f.authority.revoke(body)).proof, expected);
  assert.throws(() => f.authority.authorize(f.request(token), '/api/v1/attention'));
  await assert.rejects(f.issue());
  assert.notEqual(expected, proof(f.secret, 'revoked', f.bridgeId, f.id, 1, randomHex()));
});

test('a concurrent revocation wins over pending enrollment activation', async () => {
  const f = fixture();
  f.record.status = 'pending';
  f.store.activate = async () => { f.record.status = 'revoked'; };
  await assert.rejects(f.issue());
  assert.equal(f.authority.tokens.size, 0);
});

test('challenge issuance and rate limiter remain bounded under unknown peers and identities', () => {
  const f = fixture();
  for (let i = 0; i < 120; ++i) f.authority.challenge(f.challengeBody());
  assert.equal(f.authority.challenges.size, 1);
  assert.throws(() => f.authority.challenge(f.challengeBody()), (error) => error.statusCode === 429);
  for (let i = 0; i < 127; ++i) f.authority.rateLimit(`peer-${i}`);
  assert.throws(() => f.authority.rateLimit('overflow'), (error) => error.statusCode === 429);
  f.setNow(61_001);
  f.authority.challenge(f.challengeBody());
  assert.equal(f.authority.rates.size, 1);
});
