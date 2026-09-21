import { test, expect } from "@playwright/test";
import { nativeContext } from "./native";

test("service details distinguish failures and discard stale status", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let fail = false;
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/security")) return route.fallback();
    if (path.endsWith("/services"))
      return route.fulfill(
        fail
          ? { status: 503, json: { detail: "连接中断" } }
          : {
              json: {
                checked_at: Date.now() / 1000,
                services: [
                  {
                    id: "api",
                    name: "本机 API",
                    state: "ready",
                    detail: "请求正常",
                  },
                  {
                    id: "worker",
                    name: "任务执行器",
                    state: "unavailable",
                    detail: "任务执行器进程已停止",
                  },
                  {
                    id: "tushare",
                    name: "Tushare",
                    state: "configured",
                    detail: "仅代表配置已保存",
                  },
                  {
                    id: "trading",
                    name: "交易网关",
                    state: "not_integrated",
                    detail: "尚未接入交易柜台",
                  },
                ],
              },
            },
      );
    const user = {
      email: "services@example.com",
      first_name: "Service",
      last_name: "Test",
    };
    return route.fulfill({
      json: path.endsWith("/login")
        ? { session: "session", user }
        : path.endsWith("/me")
          ? user
          : path.endsWith("/health")
            ? { status: "ready" }
            : [],
    });
  });
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill("services@example.com");
  await page.getByLabel("密码", { exact: true }).fill("service-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page.getByRole("button", { name: /查看服务状态/ }).click();
  const panel = page.getByRole("region", { name: "服务连接详情" });
  await expect(panel.getByText("任务执行器进程已停止")).toBeVisible();
  await expect(panel.getByText("已配置 · 按需连接")).toBeVisible();
  await expect(panel.getByText("尚未接入", { exact: true })).toBeVisible();
  await page.screenshot({ path: "../../.state/service-status.png" });
  fail = true;
  await panel.getByRole("button", { name: "重新检测" }).click();
  await expect(panel.getByRole("alert")).toContainText("其他服务状态无法确认");
  await expect(panel.getByText("请求正常")).toHaveCount(0);
  await page.keyboard.press("Escape");
  await expect(panel).toHaveCount(0);
});
