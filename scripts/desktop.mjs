// Use the conventional Python 3 executable on each native build platform.
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
const script = fileURLToPath(new URL("./desktop.py", import.meta.url));
const result = spawnSync(process.platform === "win32" ? "python" : "python3", [script, ...process.argv.slice(2)], { stdio: "inherit" });
if (result.error) console.error(`Unable to start Python 3: ${result.error.message}`);
process.exit(result.status ?? 1);
