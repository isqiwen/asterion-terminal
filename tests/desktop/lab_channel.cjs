// The Electron relay to the Lab: a reply cut off after its header fails
// instead of leaving the page waiting forever.
const assert = require("node:assert");
const fs = require("node:fs");
const net = require("node:net");
const os = require("node:os");
const path = require("node:path");
const { labChannel } = require("../../apps/clients/terminal/electron/lab-channel.cjs");

const frame = body => Buffer.concat([Buffer.from([0, 0, 0, body.length]), body]);

async function lab(reply) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "ast-lab-channel-"));
  const endpoint = path.join(directory, "lab.sock");
  const server = net.createServer(socket => socket.once("data", () => reply(socket)));
  await new Promise(resolve => server.listen(endpoint, resolve));
  const close = () => {
    server.close();
    fs.rmSync(directory, { recursive: true, force: true });
  };
  return { endpoint, close };
}

const settles = (promise, ms) =>
  Promise.race([
    promise,
    new Promise((_, reject) => setTimeout(() => reject(new Error("still waiting")), ms)),
  ]);

(async () => {
  const split = await lab(socket => {
    const reply = frame(Buffer.from("reply"));
    socket.write(reply.subarray(0, 3));
    setTimeout(() => socket.end(reply.subarray(3)), 20);
  });
  const whole = await settles(labChannel(async () => split.endpoint)(Buffer.from("x")), 5000);
  assert.equal(whole.toString(), "reply");
  split.close();

  // The header announces ten bytes; two arrive before the Lab goes away.
  const cut = await lab(socket => socket.end(frame(Buffer.alloc(10)).subarray(0, 6)));
  await assert.rejects(
    settles(labChannel(async () => cut.endpoint)(Buffer.from("x")), 5000),
    /closed the connection/,
  );
  cut.close();
  console.log("lab channel relay passed");
})().catch(error => {
  console.error(error);
  process.exit(1);
});
