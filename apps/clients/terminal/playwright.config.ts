import { defineConfig } from "@playwright/test";
const origin = `http://127.0.0.1:${process.env.ASTERION_DEV_PORT ?? "1423"}`;
export default defineConfig({
  testDir: "./e2e",
  outputDir: "./test-results",
  fullyParallel: false,
  workers: 1,
  use: {
    storageState: {
      cookies: [],
      origins: [
        {
          origin: origin,
          localStorage: [{ name: "asterion.setup.completed.v1", value: "1" }],
        },
      ],
    },
    baseURL: origin,
    viewport: { width: 1440, height: 900 },
    trace: "retain-on-failure",
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
