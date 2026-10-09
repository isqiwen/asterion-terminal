import { test, expect } from "./test";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
test("native archive publishes and reads source-isolated versions", async () => {
  test.setTimeout(150000);
  const result = await promisify(execFile)(
    process.execPath,
    ["tests/desktop/electron_history_archive.cjs"],
    { timeout: 140000 },
  );
  expect(result.stdout).toContain("restart passed");
});

test("third-party C plugin drives native discovery, downloads and archive", async () => {
  test.setTimeout(150000);
  const result = await promisify(execFile)(
    process.execPath,
    ["tests/desktop/electron_native_plugin.cjs"],
    {
      timeout: 140000,
    },
  );
  expect(result.stdout).toContain("pinned restart passed");
});
