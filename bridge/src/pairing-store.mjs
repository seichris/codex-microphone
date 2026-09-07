import { createHash, X509Certificate, createPrivateKey } from 'node:crypto';
import { chmod, mkdir, mkdtemp, open, readFile, rename, rm, stat } from 'node:fs/promises';
import { join } from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { authError, proof, randomHex, validDeviceId, validGeneration, validSecret } from './device-auth.mjs';

import { acquireStoreLock } from './store-lock.mjs';

const run = promisify(execFile);
const MAX_DEVICES = 128;
const checksum = (payload) => createHash('sha256').update(payload).digest('hex');

export function validateStore(state) {
  if (!state || state.version !== 1 || !validDeviceId(state.bridgeId)
      || typeof state.certificate !== 'string' || state.certificate.length > 2047
      || typeof state.privateKey !== 'string' || !state.devices || typeof state.devices !== 'object' || Array.isArray(state.devices)
      || Object.keys(state.devices).length > MAX_DEVICES) throw new Error('Invalid pairing store');
  const certificate = new X509Certificate(state.certificate);
  if (!certificate.checkHost(`attention-${state.bridgeId}.local`)
      || !certificate.checkPrivateKey(createPrivateKey(state.privateKey))) throw new Error('Invalid bridge identity');
  for (const [id, device] of Object.entries(state.devices)) {
    if (!validDeviceId(id) || !device || typeof device !== 'object' || !validSecret(device.secret) || !validGeneration(device.generation)
        || !['pending', 'active', 'revoked'].includes(device.status)
        || (device.replaces !== null && (!validDeviceId(device.replaces) || device.replaces === id))) {
      throw new Error('Invalid device record');
    }
  }
  return state;
}

export async function atomicWrite(path, state) {
  const payload = JSON.stringify(validateStore(state));
  const envelope = `${JSON.stringify({ version: 1, payload, sha256: checksum(payload) })}\n`;
  const temporary = `${path}.${randomHex(8)}.tmp`;
  let handle;
  try {
    handle = await open(temporary, 'wx', 0o600);
    await handle.writeFile(envelope);
    await handle.sync();
    await handle.close();
    handle = null;
    await rename(temporary, path);
    const directory = await open(join(path, '..'), 'r');
    try { await directory.sync(); } finally { await directory.close(); }
  } finally {
    await handle?.close();
    await rm(temporary, { force: true });
  }
}

async function createIdentity(directory) {
  const bridgeId = randomHex(16);
  const temporary = await mkdtemp(join(directory, '.identity-'));
  try {
    await chmod(temporary, 0o700);
    const config = join(temporary, 'openssl.cnf');
    const file = await open(config, 'wx', 0o600);
    await file.writeFile(`[req]\nprompt = no\ndistinguished_name = dn\nx509_extensions = extensions\n[dn]\nCN = attention-${bridgeId}.local\n[extensions]\nsubjectAltName = DNS:attention-${bridgeId}.local\nbasicConstraints = critical,CA:TRUE\nkeyUsage = critical,digitalSignature,keyCertSign\nextendedKeyUsage = serverAuth\n`);
    await file.close();
    const key = join(temporary, 'key.pem');
    const cert = join(temporary, 'cert.pem');
    await run('openssl', ['ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', key]);
    await chmod(key, 0o600);
    await run('openssl', ['req', '-new', '-x509', '-sha256', '-days', '3650', '-key', key,
      '-out', cert, '-config', config]);
    return validateStore({ version: 1, bridgeId, certificate: await readFile(cert, 'utf8'),
      privateKey: await readFile(key, 'utf8'), devices: {} });
  } finally { await rm(temporary, { recursive: true, force: true }); }
}

export class PairingStore {
  static async open(directory) {
    await mkdir(directory, { recursive: true, mode: 0o700 });
    await chmod(directory, 0o700);
    const path = join(directory, 'pairing.json');
    const lock = await acquireStoreLock(join(directory, 'bridge.lock'));
    try {
      let state;
      try {
        const metadata = await stat(path);
        if (metadata.size > 256 * 1024) throw new Error('Pairing store too large');
        const envelope = JSON.parse(await readFile(path, 'utf8'));
        if (envelope.version !== 1 || typeof envelope.payload !== 'string'
            || envelope.sha256 !== checksum(envelope.payload)) throw new Error('Pairing store integrity failure');
        state = validateStore(JSON.parse(envelope.payload));
        await chmod(path, 0o600);
      } catch (error) {
        if (error.code !== 'ENOENT') throw error; // Never replace a corrupt identity.
        state = await createIdentity(directory);
        await atomicWrite(path, state);
      }
      return new PairingStore(path, state, lock);
    } catch (error) {
      await lock.close();
      throw error;
    }
  }

  constructor(path, state, lock = null) {
    this.path = path;
    this.state = validateStore(state);
    this.lock = lock;
    this.tail = Promise.resolve();
    this.unavailable = false;
  }
  get bridgeId() { return this.state.bridgeId; }
  get certificate() { return this.state.certificate; }
  tlsOptions() { return { key: this.state.privateKey, cert: this.state.certificate, minVersion: 'TLSv1.2' }; }
  assertAvailable() { this.lock?.assertHeld(); if (this.unavailable) throw new Error('Pairing store requires recovery'); }
  device(id) { this.assertAvailable(); const value = this.state.devices[id]; return value ? structuredClone(value) : null; }
  list() { this.assertAvailable(); return Object.entries(this.state.devices).map(([deviceId, record]) => ({
    deviceId, generation: record.generation, status: record.status,
  })); }
  mutate(change) {
    const operation = this.tail.then(async () => {
      this.assertAvailable();
      const next = structuredClone(this.state);
      const result = change(next);
      try { await atomicWrite(this.path, next); }
      catch (error) { this.unavailable = true; throw error; }
      this.lock?.assertHeld();
      this.state = next;
      return result;
    });
    this.tail = operation.catch(() => {});
    return operation;
  }
  async prepare(deviceId, replaces = null) {
    if (!validDeviceId(deviceId) || (replaces !== null && !validDeviceId(replaces))) {
      throw authError('invalid_request', 400);
    }
    return this.mutate((next) => {
      const existing = next.devices[deviceId];
      if (existing) {
        if (existing.status !== 'pending' || existing.replaces !== replaces) throw authError('device_id_conflict', 409);
        return { version: 1, deviceId, bridgeId: next.bridgeId, generation: existing.generation,
          secret: existing.secret, certificate: next.certificate };
      }
      if (Object.keys(next.devices).length >= MAX_DEVICES) throw authError('device_limit', 409);
      if (replaces !== null && !next.devices[replaces]) throw authError('unknown_previous_device', 409);
      if (replaces !== null && Object.values(next.devices).some((entry) => entry.replaces === replaces && entry.status === 'active'))
        throw authError('replace_current_device_only', 409);
      const generation = replaces === null ? 1 : next.devices[replaces].generation + 1;
      if (!validGeneration(generation)) throw authError('generation_exhausted', 409);
      const secret = randomHex();
      next.devices[deviceId] = { generation, secret, status: 'pending', replaces };
      return { version: 1, deviceId, bridgeId: next.bridgeId, generation, secret,
        certificate: next.certificate };
    });
  }
  activate(id) {
    return this.mutate((next) => {
      const record = next.devices[id];
      if (!record || record.status === 'revoked') throw authError('device_revoked_or_unknown', 403);
      if (record.replaces) {
        next.devices[record.replaces].status = 'revoked';
        for (const [otherId, other] of Object.entries(next.devices))
          if (otherId !== id && other.replaces === record.replaces && other.status === 'pending') other.status = 'revoked';
      }
      record.status = 'active';
    });
  }
  revoke(id) {
    return this.mutate((next) => {
      if (!next.devices[id]) throw authError('unknown_device', 404);
      next.devices[id].status = 'revoked';
      // Retain the now-inert key only to acknowledge an idempotent reset whose
      // response was lost. It is NEVER accepted for token issuance again.
    });
  }
  async resetAcknowledgement(id, nonce) {
    const record = this.device(id);
    if (!record || !validSecret(nonce)) throw authError('invalid_request', 400);
    await this.revoke(id);
    return { version: 1, deviceId: id, nonce,
      proof: proof(record.secret, 'revoked', this.bridgeId, id, record.generation, nonce) };
  }
  async close() {
    await this.tail;
    await this.lock?.close();
  }
}
