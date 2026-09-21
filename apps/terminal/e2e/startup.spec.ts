import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";
test("native startup retries and waits for health before showing sign in", async ({
  page, context,
}) => {
  let attempts = 0;
  await nativeContext(context, {
    desktop_info: () => null,
    desktop_session: async () => {
      attempts++;
      if (attempts === 1) throw new Error("测试：本机服务启动失败");
      return {api_url:"http://127.0.0.1:8000", token:"fixture", data_directory:"/test"};
    },
  });
  let release!: () => void;
  const health = new Promise<void>((resolve) => {
    release = resolve;
  });
  await page.route("**/api/v1/**", async (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

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
