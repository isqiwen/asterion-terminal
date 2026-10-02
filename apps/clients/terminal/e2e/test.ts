import { test as base } from "@playwright/test";
export * from "@playwright/test";

// Every spec file starts with a fresh C++ core, Agent and node directory, so
// services and data left by one file never affect the next. Specs import
// `test` from here instead of @playwright/test.
let currentFile: string | undefined;
export const test = base.extend<{
  isolatedNode: void;
  enterWorkbench: boolean;
  startupEntry: void;
}>({
  // The startup screen waits for the user on every launch. Specs about the
  // workbench pass through it; specs about startup itself set this to false.
  enterWorkbench: [true, { option: true }],
  startupEntry: [
    async ({ context, enterWorkbench }, use) => {
      if (enterWorkbench)
        context.on("page", page => {
          const enter = page.getByRole("button", { name: /^(进入工作台|ENTER WORKBENCH)$/ });
          void page.addLocatorHandler(enter, () => enter.click());
        });
      await use();
    },
    { auto: true },
  ],
  isolatedNode: [
    async ({ request }, use, testInfo) => {
      if (testInfo.file !== currentFile) {
        const response = await request.post("/__asterion/test/reset", { data: {} });
        if (!response.ok())
          throw new Error(
            `isolated node reset failed: ${response.status()} ${await response.text()}`,
          );
        process.env.ASTERION_NODE_DIRECTORY = (await response.json()).node_directory;
        // Start the local Agent and market service as the app's startup does,
        // so each file begins from a running environment as before.
        for (const method of ["node.local", "market.local"]) {
          const started = await request.post("/__asterion/api", {
            data: { version: 1, method, params: {} },
          });
          const body = await started.json();
          if (body.error) throw new Error(`${method} after reset: ${body.error.message}`);
        }
        currentFile = testInfo.file;
      }
      await use();
    },
    { auto: true },
  ],
});
