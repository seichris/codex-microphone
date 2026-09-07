import { loadConfig } from './config.mjs';
import { createBridgeServer, listen } from './http-server.mjs';
import { CodexAttentionService } from './service.mjs';
import { PairingStore } from './pairing-store.mjs';
import { DeviceAuthority } from './device-auth.mjs';
import { advertise } from './discovery.mjs';

async function main() {
  const config = await loadConfig();
  const service = new CodexAttentionService(config);
  const store = await PairingStore.open(config.pairingDirectory);
  const authority = new DeviceAuthority(store);
  const server = createBridgeServer({ service, token: config.token, authority,
    devicePort: config.devicePort, fallbackHost: config.fallbackHost });
  const deviceServer = createBridgeServer({ service, authority, device: true, tls: store.tlsOptions() });
  let stopAdvertisement = () => {};
  try {
    await listen(server, config);
    await listen(deviceServer, { host: config.deviceHost, port: config.devicePort });
    stopAdvertisement = advertise(store.bridgeId, config.devicePort);
  } catch (error) {
    server.close();
    deviceServer.close();
    await store.close();
    throw error;
  }
  console.log(`Codex ESP32 Bridge listening on http://${config.host}:${config.port}`);
  console.log(`Reading Codex Desktop state from ${service.desktopStatePath}`);
  console.log(`Paired attention HTTPS enabled on port ${config.devicePort}; use npm run pair for enrollment`);

  let shuttingDown = false;
  const shutdown = async (signal) => {
    if (shuttingDown) return;
    shuttingDown = true;
    console.log(`\n${signal}: shutting down`);
    stopAdvertisement();
    await service.stop().catch(() => console.error('Service shutdown failed'));
    await new Promise((resolvePromise) => deviceServer.close(resolvePromise));
    await new Promise((resolvePromise) => server.close(resolvePromise));
    await store.close();
  };

  process.once('SIGINT', () => void shutdown('SIGINT'));
  process.once('SIGTERM', () => void shutdown('SIGTERM'));
  try { await service.start(); }
  catch (error) { await shutdown('Startup failed'); throw error; }
}

main().catch((error) => {
  console.error(error instanceof Error ? error.stack : error);
  process.exitCode = 1;
});
