import { createReadStream } from 'node:fs';
import { spawn } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { createInterface } from 'node:readline';
import { isAbsolute, relative, resolve, sep } from 'node:path';
import { expandHome } from './util.mjs';

export class CodexAppServerError extends Error {
  constructor(message, details = undefined) {
    super(message);
    this.name = 'CodexAppServerError';
    this.details = details;
  }
}

function textFromUserInput(value) {
  if (typeof value === 'string') return value;
  if (Array.isArray(value)) return value.map(textFromUserInput).filter(Boolean).join('\n');
  if (!value || typeof value !== 'object') return '';
  if (typeof value.text === 'string') return value.text;
  if (typeof value.value === 'string') return value.value;
  if (typeof value.content === 'string') return value.content;
  if (value.content !== undefined) return textFromUserInput(value.content);
  return '';
}

function latestItemOfType(turns, itemType, newestFirst) {
  const orderedTurns = newestFirst ? turns : [...turns].reverse();
  for (const turn of orderedTurns) {
    const items = Array.isArray(turn?.items) ? turn.items : [];
    for (let index = items.length - 1; index >= 0; index -= 1) {
      const item = items[index];
      if (item?.type !== itemType) continue;
      if (itemType === 'agentMessage' || itemType === 'plan') {
        const text = textFromUserInput(item.text ?? item.content);
        if (text.trim()) return text;
      } else if (itemType === 'userMessage') {
        const text = textFromUserInput(item.content);
        if (text.trim()) return text;
      }
    }
  }
  return '';
}

/**
 * Extract the most useful latest text from App Server turn payloads.
 * Agent output wins, then a plan, then the latest user message, then preview.
 */
export function extractLatestThreadText(turns, {
  newestFirst = false,
  preview = '',
} = {}) {
  const safeTurns = Array.isArray(turns) ? turns : [];
  const agent = latestItemOfType(safeTurns, 'agentMessage', newestFirst);
  if (agent) return { kind: 'agent', text: agent };

  const plan = latestItemOfType(safeTurns, 'plan', newestFirst);
  if (plan) return { kind: 'plan', text: plan };

  const user = latestItemOfType(safeTurns, 'userMessage', newestFirst);
  if (user) return { kind: 'user', text: user };

  if (typeof preview === 'string' && preview.trim()) {
    return { kind: 'preview', text: preview };
  }
  return { kind: 'empty', text: 'No text is available for this thread yet.' };
}

function rolloutItemText(item) {
  const type = String(item?.type ?? '').toLowerCase();
  if (type === 'agentmessage' || type === 'assistantmessage' || type === 'agent') {
    const text = textFromUserInput(item.text ?? item.content);
    return text.trim() ? { kind: 'agent', text } : null;
  }
  if (type === 'plan') {
    const text = textFromUserInput(item.text ?? item.content);
    return text.trim() ? { kind: 'plan', text } : null;
  }
  if (type === 'usermessage' || type === 'user') {
    const text = textFromUserInput(item.content ?? item.text);
    return text.trim() ? { kind: 'user', text } : null;
  }
  return null;
}

function rolloutRecordItem(record, threadId) {
  const payload = record?.payload;
  if (!payload || typeof payload !== 'object') return null;
  if (threadId && typeof payload.thread_id === 'string' && payload.thread_id !== threadId) return null;

  if (payload.type === 'item_completed') return rolloutItemText(payload.item);
  if (record?.type === 'response_item' && payload.type === 'message') {
    const role = String(payload.role ?? '').toLowerCase();
    const text = textFromUserInput(payload.content);
    if (!text.trim()) return null;
    if (role === 'assistant') return { kind: 'agent', text };
    if (role === 'user') return { kind: 'user', text };
  }
  return null;
}

function allowedRolloutPath(rolloutPath, codexHome) {
  if (typeof rolloutPath !== 'string' || !isAbsolute(rolloutPath)) return false;
  if (!codexHome) return true;

  const root = resolve(codexHome);
  const candidate = resolve(rolloutPath);
  const relativePath = relative(root, candidate);
  return relativePath !== ''
    && relativePath !== '..'
    && !relativePath.startsWith(`..${sep}`)
    && !isAbsolute(relativePath);
}

/**
 * Read a Codex rollout without exposing the transcript to logs or retaining
 * the full file in memory. This is a compatibility fallback for App Server
 * versions that expose thread metadata but not paginated or embedded turns.
 */
export async function readLatestRolloutText(rolloutPath, {
  threadId = '',
  preview = '',
  codexHome = null,
  logger = console,
} = {}) {
  const fallback = extractLatestThreadText([], { preview });
  if (!allowedRolloutPath(rolloutPath, codexHome)) {
    logger?.error?.(`[bridge] rollout fallback path rejected for ${threadId || 'unknown thread'}`);
    return fallback;
  }

  const latest = { agent: '', plan: '', user: '' };
  try {
    const input = createReadStream(rolloutPath, { encoding: 'utf8' });
    const lines = createInterface({ input, crlfDelay: Infinity });
    for await (const line of lines) {
      if (!line.trim()) continue;
      let record;
      try {
        record = JSON.parse(line);
      } catch {
        continue;
      }
      const item = rolloutRecordItem(record, threadId);
      if (item?.text?.trim()) latest[item.kind] = item.text;
    }
  } catch (error) {
    logger?.error?.(
      `[bridge] rollout fallback unavailable for ${threadId || 'unknown thread'}: ${error instanceof Error ? error.message : error}`,
    );
    return fallback;
  }

  if (latest.agent.trim()) return { kind: 'agent', text: latest.agent };
  if (latest.plan.trim()) return { kind: 'plan', text: latest.plan };
  if (latest.user.trim()) return { kind: 'user', text: latest.user };
  return fallback;
}

export class CodexAppServerClient extends EventEmitter {
  #command;
  #args;
  #env;
  #cwd;
  #logger;
  #timeoutMs;
  #codexHome;
  #child = null;
  #nextId = 1;
  #pending = new Map();
  #ready = false;
  #closed = false;

  constructor({
    command = 'codex',
    args = ['app-server', '--listen', 'stdio://'],
    env = process.env,
    cwd = process.cwd(),
    logger = console,
    timeoutMs = 12_000,
    codexHome = null,
  } = {}) {
    super();
    this.#command = command;
    this.#args = args;
    this.#env = env;
    this.#cwd = cwd;
    this.#logger = logger;
    this.#timeoutMs = timeoutMs;
    const configuredCodexHome = codexHome ?? env.CODEX_HOME;
    this.#codexHome = configuredCodexHome ? resolve(expandHome(configuredCodexHome)) : null;
  }

  get ready() { return this.#ready; }
  get pid() { return this.#child?.pid ?? null; }

  async start() {
    if (this.#child) throw new CodexAppServerError('App Server client was already started');
    this.#closed = false;

    const child = spawn(this.#command, this.#args, {
      cwd: this.#cwd,
      env: this.#env,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    this.#child = child;

    child.once('error', (error) => this.#handleExit(error, child));
    child.once('exit', (code, signal) => {
      const suffix = signal ? `signal ${signal}` : `code ${code}`;
      this.#handleExit(new CodexAppServerError(`codex app-server exited with ${suffix}`), child);
    });

    createInterface({ input: child.stdout, crlfDelay: Infinity }).on('line', (line) => {
      this.#handleLine(line);
    });

    createInterface({ input: child.stderr, crlfDelay: Infinity }).on('line', (line) => {
      if (line.trim()) this.#logger.error(`[codex app-server] ${line}`);
    });

    await this.request('initialize', {
      clientInfo: {
        name: 'codex_esp32_display',
        title: 'Codex ESP32 Display',
        version: '0.3.0',
      },
      capabilities: {
        experimentalApi: true,
        optOutNotificationMethods: [
          'item/agentMessage/delta',
          'item/reasoning/textDelta',
          'item/reasoning/summaryTextDelta',
          'command/execution/outputDelta',
          'fileChange/outputDelta',
        ],
      },
    });
    await this.notify('initialized');
    this.#ready = true;
    this.emit('ready');
  }

  request(method, params = {}) {
    if (!this.#child?.stdin?.writable) {
      return Promise.reject(new CodexAppServerError('codex app-server is not writable'));
    }

    const id = this.#nextId++;
    const message = { method, id, params };

    return new Promise((resolvePromise, rejectPromise) => {
      const timer = setTimeout(() => {
        this.#pending.delete(String(id));
        rejectPromise(new CodexAppServerError(`Timed out waiting for ${method}`));
      }, this.#timeoutMs);
      timer.unref?.();

      this.#pending.set(String(id), {
        method,
        resolve: resolvePromise,
        reject: rejectPromise,
        timer,
      });

      this.#write(message).catch((error) => {
        clearTimeout(timer);
        this.#pending.delete(String(id));
        rejectPromise(error);
      });
    });
  }

  notify(method, params = undefined) {
    const message = params === undefined ? { method } : { method, params };
    return this.#write(message);
  }

  async listAllThreads({ maxThreads = 300, sectionId = undefined } = {}) {
    const threads = [];
    let cursor = null;

    while (threads.length < maxThreads) {
      const limit = Math.min(100, maxThreads - threads.length);
      const params = {
        limit,
        sortKey: 'updated_at',
        archived: false,
        sortDirection: 'desc',
      };
      if (cursor) params.cursor = cursor;
      if (sectionId !== undefined) params.sectionId = sectionId;

      const result = await this.request('thread/list', params);
      const page = Array.isArray(result?.data) ? result.data : [];
      threads.push(...page);
      cursor = result?.nextCursor ?? null;
      if (!cursor || page.length === 0) break;
    }

    return threads.slice(0, maxThreads);
  }

  async readThread(threadId, { includeTurns = false } = {}) {
    const result = await this.request('thread/read', { threadId, includeTurns });
    return result?.thread ?? null;
  }

  async listThreadTurns(threadId, {
    limit = 3,
    sortDirection = 'desc',
    itemsView = 'full',
  } = {}) {
    const result = await this.request('thread/turns/list', {
      threadId,
      limit,
      sortDirection,
      itemsView,
    });
    return Array.isArray(result?.data) ? result.data : [];
  }

  async readLatestThreadText(threadId, { preview = '' } = {}) {
    try {
      const turns = await this.listThreadTurns(threadId, {
        limit: 3,
        sortDirection: 'desc',
        itemsView: 'full',
      });
      const latest = extractLatestThreadText(turns, { newestFirst: true, preview });
      if (latest.kind !== 'preview' && latest.kind !== 'empty') return latest;
    } catch (error) {
      this.#logger.error(`[bridge] thread/turns/list fallback for ${threadId}: ${error instanceof Error ? error.message : error}`);
    }

    // Compatibility fallback for older App Server versions. Some versions
    // advertise these methods but return `paginated_threads is not supported
    // yet`; in that case metadata still includes the rollout path.
    let thread = null;
    try {
      thread = await this.readThread(threadId, { includeTurns: true });
      const latest = extractLatestThreadText(thread?.turns, {
        newestFirst: false,
        preview: thread?.preview || preview,
      });
      if (latest.kind !== 'preview' && latest.kind !== 'empty') return latest;
    } catch (error) {
      this.#logger.error(`[bridge] thread/read fallback for ${threadId}: ${error instanceof Error ? error.message : error}`);
    }

    if (!thread?.path) {
      try {
        thread = await this.readThread(threadId);
      } catch (error) {
        this.#logger.error(`[bridge] thread metadata unavailable for ${threadId}: ${error instanceof Error ? error.message : error}`);
      }
    }

    if (thread?.path) {
      return readLatestRolloutText(thread.path, {
        threadId,
        preview: thread.preview || preview,
        codexHome: this.#codexHome,
        logger: this.#logger,
      });
    }

    return extractLatestThreadText(thread?.turns, {
      newestFirst: false,
      preview: thread?.preview || preview,
    });
  }

  async close() {
    this.#closed = true;
    this.#ready = false;
    const child = this.#child;
    this.#child = null;
    if (!child) return;
    child.stdin?.end();
    if (!child.killed) child.kill('SIGTERM');
  }

  async #write(message) {
    const child = this.#child;
    if (!child?.stdin?.writable) throw new CodexAppServerError('codex app-server stdin is closed');
    const line = `${JSON.stringify(message)}\n`;
    await new Promise((resolvePromise, rejectPromise) => {
      child.stdin.write(line, (error) => error ? rejectPromise(error) : resolvePromise());
    });
  }

  #handleLine(line) {
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      this.#logger.error(`[codex app-server] ignored non-JSON stdout: ${line.slice(0, 300)}`);
      return;
    }

    if (Object.hasOwn(message, 'id') && (Object.hasOwn(message, 'result') || Object.hasOwn(message, 'error'))) {
      const pending = this.#pending.get(String(message.id));
      if (!pending) return;
      clearTimeout(pending.timer);
      this.#pending.delete(String(message.id));
      if (message.error) {
        pending.reject(new CodexAppServerError(
          `${pending.method} failed: ${message.error.message ?? 'unknown App Server error'}`,
          message.error,
        ));
      } else {
        pending.resolve(message.result);
      }
      return;
    }

    if (typeof message.method === 'string' && Object.hasOwn(message, 'id')) {
      // This observer never starts turns, so it should not receive approvals.
      // Still answer unexpected server requests so App Server cannot hang.
      void this.#write({
        id: message.id,
        error: { code: -32601, message: `Unsupported server request: ${message.method}` },
      });
      this.emit('serverRequest', message);
      return;
    }

    if (typeof message.method === 'string') {
      this.emit('notification', message);
    }
  }

  #handleExit(error, child) {
    if (this.#child !== child) return;
    this.#ready = false;
    this.#child = null;
    for (const pending of this.#pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.#pending.clear();
    if (!this.#closed) this.emit('exit', error);
  }
}
