import { defineConfig } from "@playwright/test";
const origin = `http://127.0.0.1:${process.env.ASTERION_DEV_PORT ?? "1423"}`;
export default defineConfig({
  testDir: "./e2e",
  outputDir: "./test-results",
  fullyParallel: false,
  workers: 1,
  use: {
    baseURL: origin,
    viewport: { width: 1440, height: 900 },
    trace: "retain-on-failure",
    video:
      process.env.ASTERION_E2E_VIDEO === "1"
        ? { mode: "on", size: { width: 1440, height: 900 } }
        : "off",
  },
  webServer: [
    {
      command: "pnpm --dir ../../.. dev",
      url: origin,
      reuseExistingServer: false,
      timeout: 30000,
    },
  ],
});
