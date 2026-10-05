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
        // Bring the environment up as the app's startup does (Agent, market
        // and task services, each past its first heartbeat), so the first
        // test of a file meets the same ready state as the later ones.
        const call = async (method: string, params: object = {}) => {
          const reply = await request.post("/__asterion/api", {
            data: { version: 1, method, params },
          });
          const body = await reply.json();
          if (body.error) throw new Error(`${method} after reset: ${body.error.message}`);
          return body.result;
        };
        await call("node.local");
        await call("market.local");
        const plugins = (await call("native.plugins.inspect")).native_plugins.items
          .filter(
            (item: { state: string; managed?: boolean }) =>
              item.state === "available" && !item.managed,
          )
          .map((item: { sha256: string }) => item.sha256);
        await call("node.data_tasks.local.create", { plugins });
        for (const deadline = Date.now() + 60000; ;) {
          const services = (await call("runtime.snapshot")).nodes.flatMap(
            (node: { health?: { services: { desired_running: boolean; health: string }[] } }) =>
              node.health?.services ?? [],
          );
          if (
            services.every(
              (service: { desired_running: boolean; health: string }) =>
                !service.desired_running || ["ready", "awaiting_input"].includes(service.health),
            )
          )
            break;
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
