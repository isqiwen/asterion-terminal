// Terminal development and packaging run on macOS.
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
if (process.platform !== "darwin") {
  console.error("Terminal desktop development and packaging require macOS");
  process.exit(1);
}
const script = fileURLToPath(new URL("./desktop.py", import.meta.url));
const result = spawnSync("python3", [script, ...process.argv.slice(2)], { stdio: "inherit" });
if (result.error) console.error(`Unable to start Python 3: ${result.error.message}`);
process.exit(result.status ?? 1);
