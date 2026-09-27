import { defineConfig } from "@playwright/test";
export default defineConfig({
  testDir: "./e2e", outputDir: "./test-results", fullyParallel: false, workers: 1,
  use: { storageState: { cookies: [], origins: [{ origin: "http://127.0.0.1:1420", localStorage: [{ name: "asterion.setup.completed.v1", value: "1" }] }] }, baseURL: "http://127.0.0.1:1420", viewport: { width: 1440, height: 900 }, trace: "retain-on-failure" },
  webServer: { command: "pnpm --dir ../.. dev", url: "http://127.0.0.1:1420", reuseExistingServer: false, timeout: 30000 },
});
