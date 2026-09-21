import { test, expect } from "@playwright/test";
import { nativeContext } from "./native";

test("connection rename, verification and reversible archive", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const provider = {
    id: "tushare",
    name: "Tushare Pro",
    version: "1",
    api_version: 2,
    configured: true,
    capabilities: [],
    configuration: { schema_version: 1, fields: [] },
    lifecycle: {
      id: "tushare",
      provider: "tushare",
      name: "Tushare Pro",
      state: "enabled",
      revision: 0,
    },
    verification: {
      status: "never",
      checked_at: null,
      revision: null,
      message: "尚未验证已保存配置",
    },
  };
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/security")) return route.fallback();
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [provider] });
    if (path.endsWith("/configuration"))
      return route.fulfill({
        json: {
          provider: "tushare",
          revision: 0,
          schema_version: 1,
          values: {},
          secret_fields: [],
          configured: true,
        },
      });
    if (path.endsWith("/verify")) {
      provider.verification = {
        status: "verified",
        checked_at: 1 as any,
        revision: 0 as any,
        message: "仅验证交易日历接口",
      };
      return route.fulfill({ json: provider.verification });
    }
    if (path.endsWith("/data/connections/tushare")) {
      const body = route.request().postDataJSON();
      expect(body.expected_revision).toBe(provider.lifecycle.revision);
      provider.lifecycle = {
        ...provider.lifecycle,
        ...body,
        revision: body.expected_revision + 1,
      };
      provider.name = body.name;
      return route.fulfill({ json: provider.lifecycle });
    }
    const user = {
      email: "manage@example.com",
      first_name: "Manage",
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
  await page.goto("/?screen=settings");
  await page.getByLabel("邮箱", { exact: true }).fill("manage@example.com");
  await page.getByLabel("密码", { exact: true }).fill("manage-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page.getByRole("button", { name: "数据源", exact: true }).click();
  await page.getByLabel("Tushare Pro名称").fill("研究连接");
  await page.getByRole("button", { name: "保存名称" }).click();
  await expect(page.getByLabel("研究连接名称")).toBeVisible();
  await page.getByRole("button", { name: "验证已保存配置" }).click();
  await expect(page.getByText(/验证成功/)).toBeVisible();
  await page.getByRole("button", { name: "停用", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "验证已保存配置" }),
  ).toBeDisabled();
  await page.getByRole("button", { name: "归档", exact: true }).click();
  await expect(page.getByLabel("研究连接名称")).toHaveCount(0);
  await page.getByLabel("显示归档连接").check();
  await page.getByRole("button", { name: "恢复连接" }).click();
  await expect(
    page.getByRole("button", { name: "验证已保存配置" }),
  ).toBeEnabled();
});
