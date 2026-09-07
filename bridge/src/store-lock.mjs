import { spawn } from 'node:child_process';

// POSIX advisory locks are released by the kernel, including after SIGKILL.
// A PID file cannot safely distinguish crash recovery from a concurrent writer.
// Python is already required by the local serial provisioning command. The
// helper holds no credentials and exits when its parent's stdin pipe closes.
const LOCK_HELPER = `import fcntl, os, sys
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
try:
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.exit(73)
sys.stdout.write("LOCKED\\n")
sys.stdout.flush()
sys.stdin.buffer.read()
os.close(fd)
`;

export async function acquireStoreLock(path) {
  const child = spawn('python3', ['-c', LOCK_HELPER, path], { stdio: ['pipe', 'pipe', 'ignore'] });
  let held = false;
  let closing = false;
  let exited = false;
  const exit = new Promise((resolve) => child.once('exit', () => { held = false; exited = true; resolve(); }));
  child.stdin.on('error', () => {}); // a lost helper is detected through held
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => { child.kill(); reject(new Error('Pairing store lock unavailable')); }, 5000);
    let response = '';
    const failed = () => { clearTimeout(timer); reject(new Error('Pairing store locked or lock helper unavailable')); };
    child.once('error', failed);
    child.once('exit', failed);
    child.stdout.on('data', (chunk) => {
      response += chunk.toString('utf8');
      if (response === 'LOCKED\n') {
        clearTimeout(timer);
        child.off('error', failed);
        child.off('exit', failed);
        held = true;
        resolve();
      } else if (response.length > 7) failed();
    });
  }).catch((error) => { child.kill(); throw error; });
  return {
    assertHeld() { if (!held || closing) throw new Error('Pairing store lock lost'); },
    async close() {
      if (closing) { await exit; return; }
      closing = true;
      if (!exited) child.stdin.end();
      const timer = setTimeout(() => child.kill('SIGKILL'), 2000);
      try { await exit; } finally { clearTimeout(timer); }
    },
  };
}
