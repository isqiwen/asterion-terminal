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
