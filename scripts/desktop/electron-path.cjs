// Resolve the locked runtime without loading electron/index.js, which downloads
// a binary when path.txt is absent. Runtime and test entry points never install.
const fs = require("node:fs");
const path = require("node:path");
module.exports = function electronPath() {
  const metadata = require.resolve("electron/package.json");
  const expected = require("../../package.json").devDependencies.electron;
  if (JSON.parse(fs.readFileSync(metadata, "utf8")).version !== expected)
    throw new Error("Installed Electron version differs from the workspace lock");
  const directory = path.dirname(metadata);
  const marker = path.join(directory, "path.txt");
  if (!fs.existsSync(marker))
    throw new Error("Electron runtime is missing; explicitly run pnpm exec install-electron");
  const distribution = path.join(directory, "dist");
  const executable = path.resolve(distribution, fs.readFileSync(marker, "utf8").trim());
  if (
    !executable.startsWith(distribution + path.sep) ||
    !fs.existsSync(executable) ||
    !fs.statSync(executable).isFile()
  )
    throw new Error("Electron runtime is incomplete; explicitly run pnpm exec install-electron");
  return executable;
};
