const fs = require("node:fs");
const path = require("node:path");

function diagnostics(directory, product) {
  fs.mkdirSync(directory, { recursive: true, mode: 0o700 });
  const file = path.join(
    directory,
    `electron-${process.pid}-${new Date().toISOString().slice(0, 10)}.log`,
  );
  const output = fs.openSync(file, "a", 0o600);
  return (event, error) => {
    const stack = error instanceof Error ? error.stack || error.message : String(error);
    try {
      fs.writeSync(
        output,
        `[${new Date().toISOString()}] [${event}] Asterion ${product.version} (${product.commit})\n${stack}\n`,
      );
      fs.fsyncSync(output);
    } catch (failure) {
      console.error("Cannot write desktop diagnostics:", failure, stack);
    }
  };
}
module.exports = { diagnostics };

async function processCrashReports(dumps, resources, report, signal) {
  const { spawn } = require("node:child_process");
  const files = fs.promises;
  const tools = path.join(resources, "crash");
  const cache = path.join(dumps, "symbols");
  await files.mkdir(cache, { recursive: true, mode: 0o700 });
  // Keep module IDs from earlier installations so upgrades can resolve their dumps.
  await files.cp(path.join(tools, "symbols"), cache, { recursive: true, force: false });
  const entries = await files.readdir(dumps, { recursive: true });
  for (const online of [false, true]) {
    for (const entry of entries) {
      if (!entry.endsWith(".dmp")) continue;
      const dump = path.join(dumps, entry);
      const destination = path.join(dumps, path.basename(entry) + ".report.log");
      const metadata = destination + ".json";
      const pending = value => (value.modules || []).filter(module => module.missing_symbols);
      const render = async online => {
        const temporary = destination + `.${process.pid}.tmp`;
        const structured = metadata + `.${process.pid}.tmp`;
        const errors = destination + ".errors.log";
        const output = fs.openSync(temporary, "w", 0o600);
        const stderr = fs.openSync(errors, "w", 0o600);
        try {
          const args = [
            "--cyborg",
            structured,
            "--symbols-path",
            cache,
            "--symbols-cache",
            cache,
            "--symbols-tmp",
            path.join(dumps, "downloads"),
          ];
          if (online)
            args.push(
              "--symbols-url",
              "https://symbols.electronjs.org/",
              "--symbols-download-timeout-secs",
              "60",
            );
          args.push(dump);
          await new Promise((resolve, reject) => {
            const child = spawn(path.join(tools, "minidump-stackwalk"), args, {
              stdio: ["ignore", output, stderr],
              signal,
            });
            child.once("error", reject);
            child.once("exit", code =>
              code === 0
                ? resolve()
                : reject(new Error(`Crash parser exited with ${code}: ${errors}`)),
            );
          });
          const result = JSON.parse(await files.readFile(structured, "utf8"));
          const missing = pending(result).map(module => module.debug_file || module.filename);
          fs.writeSync(
            output,
            missing.length
              ? `\nSymbols unavailable for: ${missing.join(", ")}\nUnresolved frames retain module names and addresses.\n`
              : "\nAll recorded modules have matching symbols.\n",
          );
          fs.fsyncSync(output);
          await files.rename(temporary, destination);
          await files.rename(structured, metadata);
          report("crash.report", destination);
          return result;
        } finally {
          fs.closeSync(output);
          fs.closeSync(stderr);
          await files.rm(temporary, { force: true });
          await files.rm(structured, { force: true });
        }
      };
      try {
        const available = fs.existsSync(destination) && fs.existsSync(metadata);
        if (!online && available) continue;
        if (
          online &&
          (!available || !pending(JSON.parse(await files.readFile(metadata, "utf8"))).length)
        )
          continue;
        await render(online);
      } catch (error) {
        if (signal.aborted) return;
        report("crash.report.failed", error);
      }
    }
  }
}
module.exports.processCrashReports = processCrashReports;
