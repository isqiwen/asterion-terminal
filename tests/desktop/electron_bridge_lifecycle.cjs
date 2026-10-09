// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
const assert = require("node:assert/strict");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const { Worker } = require("node:worker_threads");
const addon = path.resolve(process.argv[2]);
const bridge = require(addon);
const request = method => bridge.request(JSON.stringify({ version: 1, method, params: {} }));
(async () => {
  const failed = spawnSync(
    process.execPath,
    [
      "-e",
      `
    const assert = require('node:assert/strict');
    assert.throws(() => require(process.argv[1]), error =>
      error.code === 'recovery_required' && error.message === 'Fixture initialization requires recovery');
  `,
      addon,
    ],
    { env: { ...process.env, ASTERION_TEST_BRIDGE_INIT_FAILURE: "1" }, encoding: "utf8" },
  );
  assert.equal(failed.status, 0, failed.stderr);

  const pending = Array.from({ length: 16 }, () => request("fixture.slow"));
  let running = 0;
  for (let attempt = 0; attempt < 100 && running !== 16; attempt++) {
    running = JSON.parse(await request("runtime.snapshot")).running;
    if (running !== 16) await new Promise(resolve => setTimeout(resolve, 1));
  }
  assert.equal(
    running,
    16,
    "status lane did not respond while all admitted requests were suspended",
  );
  assert.throws(() => request("fixture.slow"), /Too many pending/);
  await Promise.all(pending);
  assert.equal(JSON.parse(await request("runtime.snapshot")).running, 0);
  await assert.rejects(
    request("fixture.fail"),
    error => error.code === "resource_exhausted" && error.message === "Fixture response failure",
  );
  assert.equal(JSON.parse(await request("runtime.snapshot")).running, 0);

  for (let attempt = 0; attempt < 3; attempt++) {
    const worker = new Worker(
      `const { parentPort, workerData } = require('node:worker_threads');
       const bridge = require(workerData);
       for (let i = 0; i < 16; i++) bridge.request(JSON.stringify({version:1,method:'fixture.slow',params:{}}));
       parentPort.postMessage('queued');`,
      { eval: true, workerData: addon },
    );
    await new Promise((resolve, reject) => {
      worker.once("error", reject);
      worker.once("message", resolve);
    });
    await worker.terminate();
  }
  console.log("Node-API lifecycle: slow request saturation, failure and forced teardown passed");
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
