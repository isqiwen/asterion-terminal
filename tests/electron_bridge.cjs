// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const assert = require("node:assert/strict");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { Worker } = require("node:worker_threads");
const addon = path.resolve(process.argv[2]);
const bridge = require(addon);
const request = method => bridge.request(JSON.stringify({ version: 1, method, params: {} }));
const snapshot = () => request("runtime.snapshot");

async function isolatedPoolCheck() {
  // Queue order with exactly one libuv worker makes this regression deterministic:
  // the old napi_async_work snapshot could only finish after PBKDF2.
  let cryptoFinished = false;
  const crypto = new Promise((resolve, reject) => {
    require("node:crypto").pbkdf2("test", "test", 2000000, 32, "sha512", error => {
      cryptoFinished = true;
      if (error) reject(error);
      else resolve();
    });
  });
  const result = JSON.parse(await snapshot());
  assert.equal(result.result.protocol, 1);
  assert.equal(cryptoFinished, false, "snapshot waited for Node's shared worker pool");
  await crypto;
}

(async () => {
  if (process.argv[3] === "pool-check") {
    await isolatedPoolCheck();
    return;
  }
  const result = JSON.parse(await snapshot());
  assert.equal(result.result.protocol, 1);
  assert.equal(result.result.phase, "ready");
  const invalid = JSON.parse(
    await bridge.request(JSON.stringify({ version: 1, method: "runtime.snapshot", params: null })),
  );
  assert.equal(invalid.error.code, "invalid_request");
  assert.throws(() => bridge.request("a".repeat(65537)), /64 KiB/);
  assert.throws(() => bridge.request("bad\0request"), /invalid character/);
  assert.equal(JSON.parse(await bridge.request("{")).error.code, "invalid_request");

  // Admission includes completed native calls whose JS callbacks have not run.
  const commands = Array.from({ length: 16 }, () => request("bridge.test.unknown"));
  assert.throws(() => request("bridge.test.unknown"), /Too many pending/);
  const statuses = Array.from({ length: 8 }, snapshot);
  assert.throws(snapshot, /Too many pending/);
  const results = await Promise.all(statuses);
  for (const value of results) assert.equal(JSON.parse(value).result.protocol, 1);
  for (const value of await Promise.all(commands)) assert.ok(JSON.parse(value).error);
  assert.equal(JSON.parse(await snapshot()).result.protocol, 1);
  assert.ok(JSON.parse(await request("bridge.test.unknown")).error);

  await new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [__filename, addon, "pool-check"], {
      env: { ...process.env, UV_THREADPOOL_SIZE: "1" },
      stdio: "inherit",
    });
    child.once("error", reject);
    child.once("exit", code => {
      if (code === 0) resolve();
      else reject(new Error(`Shared-pool regression failed: ${code}`));
    });
  });

  // Forced environment teardown must finalize callbacks and join native workers.
  for (let attempt = 0; attempt < 5; attempt++) {
    const worker = new Worker(
      `const { parentPort, workerData } = require('node:worker_threads');
       const bridge = require(workerData);
       for (let i = 0; i < 16; i++) bridge.request(JSON.stringify({version:1,method:'bridge.test.unknown',params:{}}));
       parentPort.postMessage('queued');`,
      { eval: true, workerData: addon },
    );
    await new Promise((resolve, reject) => {
      worker.once("error", reject);
      worker.once("message", resolve);
    });
    await worker.terminate();
  }
  console.log(
    "Node-API bridge: real C++ responses, bounded admission, independent status reads and environment teardown passed",
  );
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
