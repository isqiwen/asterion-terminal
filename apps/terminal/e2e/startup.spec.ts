import { expect, test } from "@playwright/test";
test("native startup retries and waits for health before showing sign in", async ({
  page,
}) => {
  await page.addInitScript(() => {
    let attempts = 0;
    Object.assign(window, {
      isTauri: true,
      __TAURI_INTERNALS__: {
        invoke: async (command: string) => {
          if (command === "desktop_workspace_read")
            return {
              revision: 0,
              id: "main",
              layout: { version: 4, view: "市场" },
            };
          if (command === "desktop_account_read")
            return { revision: 0, token: null };
          if (command === "desktop_window_id") return "main";
          if (command === "desktop_info") return null;
          if (command === "desktop_session") {
            attempts++;
            await new Promise((resolve) => setTimeout(resolve, 100));
            if (attempts === 1) throw new Error("测试：本机服务启动失败");
            return {
              api_url: "http://127.0.0.1:8000",
              token: "fixture",
              data_directory: "/test",
            };
          }
        },
      },
    });
  });
  let release!: () => void;
  const health = new Promise<void>((resolve) => {
    release = resolve;
  });
  await page.route("**/api/v1/**", async (route) => {
    if (route.request().url().endsWith("/health")) await health;
    await route.fulfill({ json: { status: "ready" } });
  });
  await page.goto("/");
  await expect(page.getByRole("alert")).toContainText("本机服务启动失败");
  await page.getByRole("button", { name: "重试启动" }).click();
  await expect(
    page.getByRole("progressbar", { name: "启动本机服务" }),
  ).toHaveAttribute("aria-valuenow", "100");
  await expect(page.getByRole("main", { name: "账号入口" })).toHaveCount(0);
  await page.screenshot({ path: "../../.state/startup-reference.png" });
  release();
  await expect(
    page.getByRole("heading", { name: "登录", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("navigation", { name: "业务工作区" }),
  ).toHaveCount(0);
});
