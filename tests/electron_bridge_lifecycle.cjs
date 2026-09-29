const assert = require("node:assert/strict");
const path = require("node:path");
const { Worker } = require("node:worker_threads");
const addon = path.resolve(process.argv[2]);
const bridge = require(addon);
const request = method => bridge.request(JSON.stringify({ version: 1, method, params: {} }));
(async () => {
  const pending = Array.from({ length: 16 }, () => request("fixture.slow"));
  let running = 0;
  for (let attempt = 0; attempt < 100 && running !== 4; attempt++) {
    running = JSON.parse(await request("runtime.snapshot")).running;
    if (running !== 4) await new Promise(resolve => setTimeout(resolve, 1));
  }
  assert.equal(running, 4, "status lane did not respond while every request worker was busy");
  assert.throws(() => request("fixture.slow"), /Too many pending/);
  await Promise.all(pending);
  assert.equal(JSON.parse(await request("runtime.snapshot")).running, 0);
  await assert.rejects(request("fixture.fail"), /Cannot allocate the C\+\+ response/);
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
