import { randomUUID } from 'node:crypto';
import { mkdir, readFile, rename, unlink, writeFile } from 'node:fs/promises';
import { join } from 'node:path';

const VOICE_STATES = new Set([
  'ready',
  'focusing',
  'starting',
  'listening',
  'muted',
  'error',
  'unknown',
]);
const FOCUS_CONFIDENCES = new Set(['confirmed', 'inferred', 'unavailable']);
const VOICE_COMMANDS = new Set(['start-or-resume', 'mute']);
const WIRELESS_STATES = new Set(['idle', 'starting', 'listening', 'stopping', 'error']);

export class DesktopControlError extends Error {
  constructor(code, message = code, statusCode = 503) {
    super(message);
    this.name = 'DesktopControlError';
    this.code = code;
    this.statusCode = statusCode;
  }
}
export function validOpaqueId(value) {
  return typeof value === 'string'
    && value.length >= 16
    && value.length <= 160
    && /^[A-Za-z0-9_-]+$/.test(value);
}

export function validRequestId(value) {
  return typeof value === 'string'
    && value.length >= 1
    && value.length <= 96
    && /^[A-Za-z0-9._:-]+$/.test(value);
}

export function unavailableDesktopState() {
  return {
    available: false,
    threadId: null,
    focusConfidence: 'unavailable',
    voiceState: 'unknown',
    capabilities: {
      desktopFocus: false,
      desktopVoiceHotkey: false,
      powerButtonLongPress: false,
      wirelessMicrophone: false,
    },
    wirelessSession: {
      sessionId: null,
      transport: null,
      state: 'idle',
      revision: 0,
      errorCode: null,
    },
  };
}

export function normalizeDesktopState(value) {
  const unavailable = unavailableDesktopState();
  if (!value || typeof value !== 'object') return unavailable;
  const threadId = validOpaqueId(value.threadId) ? value.threadId : null;
  const focusConfidence = FOCUS_CONFIDENCES.has(value.focusConfidence)
    ? value.focusConfidence
    : (threadId ? 'inferred' : 'unavailable');
  const voiceState = VOICE_STATES.has(value.voiceState) ? value.voiceState : 'unknown';
  const capabilities = value.capabilities && typeof value.capabilities === 'object'
    ? value.capabilities
    : {};
  const wireless = value.wirelessSession && typeof value.wirelessSession === 'object'
    ? value.wirelessSession
    : {};
  const candidateWirelessSessionId = wireless.sessionID ?? wireless.sessionId;
  const wirelessSessionId = typeof candidateWirelessSessionId === 'string' && candidateWirelessSessionId.length <= 96
    ? candidateWirelessSessionId
    : null;
  const wirelessTransport = wireless.transport === 'wifi' ? 'wifi' : null;
  const wirelessState = WIRELESS_STATES.has(wireless.state) ? wireless.state : 'idle';
  const wirelessRevision = Number.isSafeInteger(wireless.revision) && wireless.revision >= 0
    ? wireless.revision
    : 0;
  const wirelessError = typeof wireless.errorCode === 'string' && wireless.errorCode.length <= 64
    ? wireless.errorCode
    : null;
  return {
    available: value.available === true,
    threadId,
    focusConfidence: threadId ? focusConfidence : 'unavailable',
    voiceState,
    capabilities: {
      desktopFocus: capabilities.desktopFocus === true,
      desktopVoiceHotkey: capabilities.desktopVoiceHotkey === true,
      powerButtonLongPress: capabilities.powerButtonLongPress === true,
      wirelessMicrophone: capabilities.wirelessMicrophone === true,
    },
    wirelessSession: {
      sessionId: wirelessSessionId,
      transport: wirelessTransport,
      state: wirelessState,
      revision: wirelessRevision,
      errorCode: wirelessError,
    },
  };
}

// The companion's state file can be observed out of order around an IPC
// request. Never let a lower wireless revision erase a newer session summary.
export function mergeDesktopState(previous, next) {
  const oldState = normalizeDesktopState(previous);
  const newState = normalizeDesktopState(next);
  if (newState.available && newState.wirelessSession.revision < oldState.wirelessSession.revision) {
    return { ...newState, wirelessSession: { ...oldState.wirelessSession } };
  }
  return newState;
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

export class DesktopControllerClient {
  #directory;
  #token;
  #timeoutMs;
  #pollMs;
  #serial = Promise.resolve();
  #completed = new Map();

  constructor({
    directory = process.env.CODEX_DESKTOP_CONTROL_DIR ?? '',
    token = process.env.CODEX_DESKTOP_CONTROL_TOKEN ?? '',
    timeoutMs = 2500,
    pollMs = 25,
  } = {}) {
    this.#directory = directory;
    this.#token = token;
    this.#timeoutMs = timeoutMs;
    this.#pollMs = pollMs;
  }

  get available() {
    return Boolean(this.#directory && this.#token.length >= 24);
  }

  async state() {
    if (!this.available) return unavailableDesktopState();
    return normalizeDesktopState({ ...await this.#enqueue('state', {}), available: true });
  }

  async focus(threadId, requestId) {
    if (!validOpaqueId(threadId) || !validRequestId(requestId)) {
      throw new DesktopControlError('invalid_request', 'Invalid threadId or requestId.', 400);
    }
    return this.#idempotent(requestId, `focus:${threadId}`, async () => (
      normalizeDesktopState({ ...await this.#enqueue('focus', { threadId }), available: true })
    ));
  }

  async voice(threadId, command, requestId) {
    if (!validOpaqueId(threadId) || !validRequestId(requestId) || !VOICE_COMMANDS.has(command)) {
      throw new DesktopControlError('invalid_request', 'Invalid threadId, command, or requestId.', 400);
    }
    return this.#idempotent(requestId, `voice:${threadId}:${command}`, async () => (
      normalizeDesktopState({ ...await this.#enqueue('voice', { threadId, command }), available: true })
    ));
  }

  #idempotent(requestId, signature, operation) {
    const existing = this.#completed.get(requestId);
    if (existing) {
      if (existing.signature !== signature) {
        throw new DesktopControlError(
          'request_id_reused',
          'requestId was already used for a different command.',
          409,
        );
      }
      return existing.promise;
    }
    const promise = operation();
    this.#completed.set(requestId, { signature, promise, createdAt: Date.now() });
    for (const [id, entry] of this.#completed) {
      if (Date.now() - entry.createdAt > 5 * 60_000) this.#completed.delete(id);
    }
    return promise;
  }

  #enqueue(operation, parameters) {
    if (!this.available) {
      throw new DesktopControlError(
        'desktop_control_unavailable',
        'The macOS Desktop Voice controller is unavailable.',
      );
    }
    const work = this.#serial.then(() => this.#exchange(operation, parameters));
    this.#serial = work.catch(() => {});
    return work;
  }

  async #exchange(operation, parameters) {
    const requestDirectory = join(this.#directory, 'requests');
    const responseDirectory = join(this.#directory, 'responses');
    await mkdir(requestDirectory, { recursive: true, mode: 0o700 });
    await mkdir(responseDirectory, { recursive: true, mode: 0o700 });

    const ipcId = randomUUID();
    const temporaryPath = join(requestDirectory, `.${ipcId}.tmp`);
    const requestPath = join(requestDirectory, `${ipcId}.json`);
    const responsePath = join(responseDirectory, `${ipcId}.json`);
    const request = {
      version: 1,
      token: this.#token,
      ipcId,
      operation,
      ...parameters,
    };
    await writeFile(temporaryPath, `${JSON.stringify(request)}\n`, { mode: 0o600, flag: 'wx' });
    await rename(temporaryPath, requestPath);

    const deadline = Date.now() + this.#timeoutMs;
    while (Date.now() < deadline) {
      try {
        const raw = await readFile(responsePath, 'utf8');
        await unlink(responsePath).catch(() => {});
        const response = JSON.parse(raw);
        if (response?.ipcId !== ipcId || response?.version !== 1) {
          throw new DesktopControlError('desktop_control_invalid_response');
        }
        if (response.ok !== true) {
          throw new DesktopControlError(
            typeof response.error === 'string' ? response.error : 'desktop_control_failed',
            typeof response.message === 'string' ? response.message : 'Desktop control failed.',
            Number.isInteger(response.statusCode) ? response.statusCode : 503,
          );
        }
        return response.state;
      } catch (error) {
        if (error?.code !== 'ENOENT') {
          await unlink(requestPath).catch(() => {});
          if (error instanceof DesktopControlError) throw error;
          throw new DesktopControlError('desktop_control_invalid_response', String(error));
        }
      }
      await delay(this.#pollMs);
    }

    await unlink(requestPath).catch(() => {});
    throw new DesktopControlError('desktop_control_timeout', 'Desktop controller did not respond.');
  }
}
