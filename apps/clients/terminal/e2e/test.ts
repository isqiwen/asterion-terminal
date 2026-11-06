import { test as base } from "@playwright/test";
export * from "@playwright/test";

// End-to-end tests drive the real core and services through the UI.
// What a screen does with a given reply belongs in a component test beside
// its source (*.test.tsx), which needs none of this. The tests tagged
// @journey are the complete paths a user depends on, one per capability;
// `pnpm test:e2e:journeys` runs only those.
//
// Every spec file starts with a fresh C++ core and root directory, so
// services and data left by one file never affect the next. Specs import
// `test` from here instead of @playwright/test.
let currentFile: string | undefined;
export const test = base.extend<{
  isolatedNode: void;
  enterWorkbench: boolean;
  startupEntry: void;
}>({
  // The startup screen waits for the user while it has something to point
  // out. Specs about the workbench pass through it; specs about startup
  // itself set this to false.
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
        process.env.ASTERION_ROOT = (await response.json()).root;
        // Attach the new Terminal to the independently supervised Market.
        // This creates no local Market and changes no provider state.
        const call = async (method: string, params: object = {}) => {
          const reply = await request.post("/__asterion/api", {
            data: { version: 1, method, params },
          });
          const body = await reply.json();
          if (body.error) throw new Error(`${method} after reset: ${body.error.message}`);
          return body.result;
        };
        await call("history.endpoint.save", {
          host: "127.0.0.1",
          port: Number(process.env.ASTERION_TEST_HISTORY_PORT),
          fingerprint: process.env.ASTERION_TEST_HISTORY_FINGERPRINT!,
        });
        await call("quotes.endpoint.save", {
          host: "127.0.0.1",
          port: Number(process.env.ASTERION_TEST_QUOTES_PORT),
          fingerprint: process.env.ASTERION_TEST_QUOTES_FINGERPRINT!,
        });
        await call("quotes.open");
        for (const deadline = Date.now() + 60000; ;) {
          const { lab } = await call("runtime.snapshot");
          if (!lab || lab.state === "ready") break;
          if (Date.now() > deadline) throw new Error("services did not become healthy after reset");
          await new Promise(resolve => setTimeout(resolve, 300));
        }
        currentFile = testInfo.file;
      }
      await use();
    },
    // Its own time limit: bringing the services up does not eat into the
    // time of the first test of a file.
    { auto: true, timeout: 90000 },
  ],
});
