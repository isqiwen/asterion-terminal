import { test, expect } from "@playwright/test";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import type { Snapshot } from "../src/bridge/client";

for (const state of ["update_available", "recovery_required"] as const) {
  test(`settings coordinates ${state} without manually stopping running services`, async ({
    page,
  }) => {
    await page.goto("/");
    const settings = await openSettingsWindow(page);
    await settings.getByRole("button", { name: "连接与部署", exact: true }).click();
    await settings.getByText("Agent 程序", { exact: true }).click();
    let inspected: Snapshot | undefined;
    const calls: string[] = [];
    await settings.route("**/__asterion/api", async route => {
      const request = route.request().postDataJSON();
      calls.push(request.method);
      if (request.method === "node.agent.inspect") {
        const response = await route.fetch();
        const data = (await response.json()) as { result: Snapshot };
        inspected = data.result;
        expect(inspected.nodes[0].health?.services.some(service => service.desired_running)).toBe(
          true,
        );
        inspected.agent_program = {
          state,
          expected_digest: "1".repeat(64),
          installed_digest: "1".repeat(64),
          bundled_digest: "2".repeat(64),
        };
        return route.fulfill({ response, json: { result: inspected } });
      }
      if (request.method === "node.agent.upgrade") {
        expect(request.params).toEqual({ expected_digest: "1".repeat(64) });
        expect(inspected).toBeDefined();
        return route.fulfill({
          json: {
            result: {
              ...inspected,
              agent_program: {
                state: "current",
                expected_digest: "",
                installed_digest: "2".repeat(64),
                bundled_digest: "2".repeat(64),
              },
            },
          },
        });
      }
      return route.continue();
    });
    await settings.getByRole("button", { name: "检查程序更新", exact: true }).click();
    await expect(settings.getByText(/更新会等待服务安全停止/)).toBeVisible();
    const upgrade = settings.getByRole("button", {
      name: state === "recovery_required" ? "继续恢复" : "升级 Agent",
      exact: true,
    });
    await expect(upgrade).toBeEnabled();
    await upgrade.click();
    await expect(settings.getByText("已安装程序与安装包一致", { exact: true })).toBeVisible();
    expect(calls.filter(method => method === "node.agent.upgrade")).toHaveLength(1);
    expect(calls).not.toContain("node.action");
    await closeSettingsWindow(settings);
  });
}

test("settings shows a busy command without automatically retrying or losing drafts", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据", exact: true }).click();
  await page.getByLabel("品种代码").fill("CU");
  const settings = await openSettingsWindow(page);
  await settings.getByRole("button", { name: "连接与部署", exact: true }).click();
  await settings.getByText("Agent 程序", { exact: true }).click();
  let inspections = 0;
  await settings.route("**/__asterion/api", route => {
    if (route.request().postDataJSON().method === "node.agent.inspect" && ++inspections === 1) {
      return route.fulfill({
        contentType: "application/json",
        body: JSON.stringify({
          error: {
            code: "conflict",
            message: "another Terminal operation is in progress; retry after it completes",
          },
        }),
      });
    }
    return route.continue();
  });
  const inspect = settings.getByRole("button", { name: "检查程序更新", exact: true });
  await inspect.click();
  await expect(settings.getByRole("alert")).toContainText("已有操作正在进行，请完成后重试");
  await expect(inspect).toBeEnabled();
  expect(inspections).toBe(1);
  await expect(page.getByLabel("品种代码")).toHaveValue("CU");
  await inspect.click();
  await expect(settings.getByRole("alert")).toHaveCount(0);
  expect(inspections).toBe(2);
  await closeSettingsWindow(settings);
});

test("settings reuses its window and preserves workbench drafts", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据", exact: true }).click();
  await page.getByLabel("品种代码").fill("CU");
  const settings = await openSettingsWindow(page);
  await settings.setViewportSize({ width: 760, height: 540 });
  await expect(settings.getByRole("heading", { name: "偏好设置", exact: true })).toBeVisible();
  await expect(page.getByLabel("品种代码")).toHaveValue("CU");
  await settings.getByLabel("语言", { exact: true }).selectOption("en-US");
  await expect(page.locator("html")).toHaveAttribute("lang", "en-US");
  await settings.getByLabel("Display Density").selectOption("comfortable");
  await settings.screenshot({ path: "apps/clients/terminal/test-results/settings-window.png" });
  await settings.getByRole("button", { name: "About", exact: true }).click();
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await expect(settings.getByRole("heading", { name: "About", exact: true })).toBeVisible();
  await settings.keyboard.press("Control+,");
  await expect(settings.getByRole("heading", { name: "About", exact: true })).toBeVisible();
  await page.getByLabel("Product Code").fill("RB");
  await settings.getByRole("button", { name: "Preferences", exact: true }).click();
  expect(page.context().pages()).toHaveLength(2);
  await settings.getByLabel("Language").selectOption("zh-CN");
  await page.getByRole("button", { name: "查看服务连接", exact: true }).click();
  await page.getByRole("button", { name: "连接设置", exact: true }).click();
  await expect(settings.getByRole("heading", { name: "连接与部署", exact: true })).toBeVisible();
  expect(page.context().pages()).toHaveLength(2);
  await settings.screenshot({
    path: "apps/clients/terminal/test-results/settings-connections.png",
  });
  expect(await settings.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
    true,
  );
  await closeSettingsWindow(settings);
  await expect(page.getByLabel("品种代码")).toHaveValue("RB");
  const reopened = await openSettingsWindow(page);
  await expect(reopened.getByRole("heading", { name: "连接与部署", exact: true })).toBeVisible();
  await reopened.setViewportSize({ width: 640, height: 440 });
  expect(await reopened.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
    true,
  );
  await closeSettingsWindow(reopened);
});
