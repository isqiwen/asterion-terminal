const path = require("node:path");
const { spawn } = require("node:child_process");
const root = path.resolve(__dirname, "../..");
const electron = require("./electron-path.cjs")();
const env = { ...process.env };
delete env.ELECTRON_RUN_AS_NODE;
const child = spawn(electron, [path.join(root, "apps/clients/installer/electron")], {
  env,
  stdio: "inherit",
});
child.on("error", error => {
  console.error(error.message);
  process.exitCode = 1;
});
child.on("exit", code => {
  process.exitCode = code ?? 1;
});
