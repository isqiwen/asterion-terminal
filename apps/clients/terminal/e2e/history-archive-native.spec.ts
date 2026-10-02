import { test, expect } from "./test";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
test("macOS native archive publishes and reads source-isolated versions", async () => {
  test.skip(process.platform !== "darwin", "macOS Terminal only");
  test.setTimeout(150000);
  const result = await promisify(execFile)(
    process.execPath,
    ["tests/electron_history_archive.cjs"],
    { timeout: 140000 },
  );
  expect(result.stdout).toContain("restart passed");
});

test("third-party C plugin drives native discovery, downloads and archive", async () => {
  test.skip(process.platform !== "darwin", "macOS Terminal only");
  test.setTimeout(150000);
  const result = await promisify(execFile)(process.execPath, ["tests/electron_native_plugin.cjs"], {
    timeout: 140000,
  });
  expect(result.stdout).toContain("pinned restart passed");
});
