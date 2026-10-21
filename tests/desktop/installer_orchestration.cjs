// Exercise Electron's maintenance ordering without launching programs or changing services.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { EventEmitter } = require("node:events");
const { PassThrough, Writable } = require("node:stream");
const { pathToFileURL } = require("node:url");
const root = path.resolve(__dirname, "../..");
const handlers = new Map();
let window;
class BrowserWindow {
  constructor() {
    window = this;
    this.webContents = {
      mainFrame: { url: "" },
      setWindowOpenHandler() {},
      on() {},
      send() {},
    };
  }
  on() {}
  loadFile(file) {
    this.webContents.mainFrame.url = pathToFileURL(file).href;
  }
}
let calls = [],
  failure = "",
  pending = "";
function spawn(program, args) {
  const action = args[args.indexOf("--operation") + 1];
  const child = new EventEmitter();
  child.stdout = new PassThrough();
  child.stderr = new PassThrough();
  let input = "";
  child.stdin = new Writable({
    write(data, _encoding, done) {
      input += data;
      done();
    },
    final(done) {
      const request = input ? JSON.parse(input) : {};
      const operation = action === "remote" ? request.maintenance.action : action;
      calls.push(operation);
      assert(!args.some(arg => arg.includes("privateKey")), "credentials must travel over stdin");
      queueMicrotask(() => {
        if (operation === failure) {
          child.stdout.end(
            JSON.stringify({ error: { code: "unavailable", message: "fixture failure" } }) + "\n",
          );
          child.emit("close", 1);
        } else {
          if (["repair", "upgrade", "uninstall"].includes(action)) pending = "";
          const result =
            action === "remote"
              ? { nodes: [], pending: null }
              : { installed: true, pending, node: "/fixture/node" };
          child.stdout.end(JSON.stringify({ result }) + "\n");
          child.emit("close", 0);
        }
      });
      done();
    },
  });
  return child;
}
const electron = {
  app: {
    isPackaged: false,
    requestSingleInstanceLock: () => true,
    on() {},
    whenReady: () => Promise.resolve(),
    getPath: () => "/fixture/home",
  },
  BrowserWindow,
  ipcMain: { handle: (name, fn) => handlers.set(name, fn) },
  shell: {},
};
vm.runInNewContext(
  fs.readFileSync(path.join(root, "apps/clients/installer/electron/main.cjs"), "utf8"),
  {
    require: name =>
      name === "electron" ? electron : name === "node:child_process" ? { spawn } : require(name),
    __dirname: path.join(root, "apps/clients/installer/electron"),
    process: {
      platform: "linux",
      arch: "x64",
      env: { ASTERION_INSTALL_SOURCE: "/fixture/payload" },
    },
  },
);
(async () => {
  await new Promise(resolve => setImmediate(resolve));
  const event = { sender: window.webContents, senderFrame: window.webContents.mainFrame };
  const run = (action, remove = false, all = false) =>
    handlers.get("installer:run")(event, action, remove, all);
  await run("upgrade");
  assert.deepEqual(calls, ["inspect", "PREPARE", "stop", "upgrade", "FINISH", "inspect"]);
  calls = [];
  failure = "PREPARE";
  await assert.rejects(run("upgrade"), /fixture failure/);
  assert.deepEqual(calls, ["inspect", "PREPARE"]);
  calls = [];
  failure = "upgrade";
  await assert.rejects(run("upgrade"), /fixture failure/);
  assert.deepEqual(calls, ["inspect", "PREPARE", "stop", "upgrade"]);
  calls = [];
  failure = "UNINSTALL";
  await assert.rejects(run("uninstall", true, true), /fixture failure/);
  assert.deepEqual(calls, ["inspect", "UNINSTALL"]);
  calls = [];
  failure = "";
  pending = "upgrade";
  await run("upgrade");
  assert.deepEqual(calls, ["inspect", "upgrade", "FINISH", "inspect"]);
  calls = [];
  await run("uninstall", false, true);
  assert.deepEqual(calls, ["inspect", "UNINSTALL", "stop", "uninstall", "inspect"]);
  calls = [];
  pending = "uninstall";
  await run("uninstall", true, true);
  assert.deepEqual(calls, ["inspect", "uninstall", "inspect"]);
  calls = [];
  failure = "PREPARE";
  await run("repair");
  assert.deepEqual(calls, ["inspect", "repair", "inspect"]);
  console.log(
    "Installer remote-first ordering, failure boundaries and interrupted local publication verified",
  );
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
