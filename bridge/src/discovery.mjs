import { spawn } from 'node:child_process';
import { DEVICE_SERVICE, validDeviceId } from './device-auth.mjs';

export function advertisement(bridgeId, port) {
  if (!validDeviceId(bridgeId) || !Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error('Invalid attention advertisement');
  }
  return [`attention-${bridgeId}`, DEVICE_SERVICE, 'local.', String(port), `id=${bridgeId}`, 'v=1', 'tls=1'];
}

// Use the system Bonjour daemon; no second multicast responder or dependency.
export function advertise(bridgeId, port, { logger = console, platform = process.platform, spawnProcess = spawn } = {}) {
  if (platform !== 'darwin') {
    logger.warn('Attention discovery unavailable: use the paired hostname fallback');
    return () => {};
  }
  let stopped = false;
  let timer;
  let child;
  let delay = 1000;
  const start = () => {
    child = spawnProcess('/usr/bin/dns-sd', ['-R', ...advertisement(bridgeId, port)], { stdio: 'ignore' });
    let scheduled = false;
    const retry = () => {
      if (stopped || scheduled) return;
      scheduled = true;
      logger.warn('Attention discovery unavailable; retrying registration');
      timer = setTimeout(start, delay);
      timer.unref?.();
      delay = Math.min(delay * 2, 30_000);
    };
    child.once('error', retry);
    child.once('exit', retry);
  };
  start();
  return () => { stopped = true; clearTimeout(timer); child?.kill('SIGTERM'); };
}
