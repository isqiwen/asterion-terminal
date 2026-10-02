// Terminal development and packaging run on macOS.
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
if (process.platform !== "darwin") {
  console.error("Terminal desktop development and packaging require macOS");
  process.exit(1);
}
const script = fileURLToPath(new URL("./desktop.py", import.meta.url));
const child = spawn("python3", [script, ...process.argv.slice(2)], {
  stdio: "inherit",
  detached: true,
});
for (const signal of ["SIGINT", "SIGTERM"])
  process.on(signal, () => {
    try {
      process.kill(-child.pid, signal);
    } catch (error) {
      if (error.code !== "ESRCH") throw error;
    }
  });
child.once("error", error => {
  console.error(`Unable to start Python 3: ${error.message}`);
  process.exitCode = 1;
});
child.once("exit", code => {
  process.exitCode = code ?? 1;
});
