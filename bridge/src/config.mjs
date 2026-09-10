import { accessSync, constants } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { delimiter, dirname, isAbsolute, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { homedir, hostname } from 'node:os';
import { normalizeAttentionFilter } from './attention.mjs';
import { expandHome } from './util.mjs';

const here = dirname(fileURLToPath(import.meta.url));


function isExecutable(path) {
  try {
    accessSync(path, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

export function resolveCodexBin(configured = 'codex', env = process.env, platform = process.platform) {
  if (configured !== 'codex') {
    if (configured === '~' || configured.startsWith('~/') || isAbsolute(configured)) {
      return expandHome(configured);
    }
    return configured;
  }

  const names = platform === 'win32' ? ['codex.exe', 'codex.cmd', 'codex.bat'] : ['codex'];
  for (const directory of String(env.PATH ?? '').split(delimiter).filter(Boolean)) {
    for (const name of names) {
      const candidate = join(directory, name);
      if (isExecutable(candidate)) return candidate;
    }
  }

  if (platform === 'darwin') {
    const candidates = [
      '/Applications/ChatGPT.app/Contents/Resources/codex',
      '/Applications/Codex.app/Contents/Resources/codex',
      join(homedir(), 'Applications/ChatGPT.app/Contents/Resources/codex'),
      join(homedir(), 'Applications/Codex.app/Contents/Resources/codex'),
      join(homedir(), '.local/bin/codex'),
    ];
    for (const candidate of candidates) {
      if (isExecutable(candidate)) return candidate;
    }
  }

  return configured;
}

function integer(value, fallback, min, max) {
  const parsed = Number.parseInt(String(value ?? ''), 10);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.min(max, Math.max(min, parsed));
}

export async function loadConfig() {
  const configPath = resolve(
    process.env.CODEX_ATTENTION_CONFIG ?? resolve(here, '..', 'config.json'),
  );

  let fileConfig;
  try {
    fileConfig = JSON.parse(await readFile(configPath, 'utf8'));
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    throw new Error(`Could not read ${configPath}: ${message}. Run \"npm run setup\" first.`);
  }

  const config = {
    configPath,
    host: '127.0.0.1', // Legacy LAN host is deliberately not reused for administration.
    deviceHost: fileConfig.deviceHost ?? '0.0.0.0',
    devicePort: integer(fileConfig.devicePort, 5182, 1, 65535),
    pairingDirectory: resolve(dirname(configPath), '.attention-pairing'),
    fallbackHost: fileConfig.fallbackHost ?? hostname(),
    port: integer(process.env.CODEX_ATTENTION_PORT ?? fileConfig.port, 5180, 1, 65535),
    token: process.env.CODEX_ATTENTION_TOKEN ?? fileConfig.token ?? '',
    pollIntervalMs: integer(
      process.env.CODEX_ATTENTION_POLL_MS ?? fileConfig.pollIntervalMs,
      2000,
      500,
      60_000,
    ),
    maxThreads: integer(fileConfig.maxThreads, 300, 25, 2000),
    maxItems: integer(fileConfig.maxItems, 30, 1, 100),
    attentionFilter: normalizeAttentionFilter(
      process.env.CODEX_ATTENTION_FILTER ?? fileConfig.attentionFilter,
    ),
    codexBin: resolveCodexBin(process.env.CODEX_BIN ?? fileConfig.codexBin ?? 'codex'),
    codexHome: expandHome(process.env.CODEX_HOME ?? fileConfig.codexHome ?? '~/.codex'),
    desktopControlDirectory: process.env.CODEX_DESKTOP_CONTROL_DIR ?? '',
    desktopControlToken: process.env.CODEX_DESKTOP_CONTROL_TOKEN ?? '',
  };

  if (!config.token) {
    throw new Error('A local administrative token is required. Run "npm run setup".');
  }
  if (config.token && config.token.length < 24) {
    throw new Error('Bridge token must be at least 24 characters.');
  }
  if (typeof config.fallbackHost !== 'string' || config.fallbackHost.length > 253
      || (config.fallbackHost !== '' && !config.fallbackHost.split('.').every((label) =>
        /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/.test(label)))) {
    throw new Error('Invalid fallback hostname');
  }
  if (config.devicePort === config.port || config.devicePort === 5181) {
    throw new Error('Attention TLS port must be separate from admin and wireless microphone ports');
  }
  return config;
}
