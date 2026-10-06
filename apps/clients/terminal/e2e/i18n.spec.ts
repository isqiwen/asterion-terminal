import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";
import { registerLanguageResources, translate } from "../src/i18n";

test("language resources validate namespaces, keys and interpolation", () => {
  const catalog = { "zh-CN": { hello: "你好 {name}" }, "en-US": { hello: "Hello {name}" } };
  registerLanguageResources("test.languages", catalog);
  expect(translate("test.languages", "hello", { name: "Asterion" })).toBe("你好 Asterion");
  expect(() => translate("test.languages", "hello")).toThrow("argument");
  expect(() => translate("test.languages", "missing")).toThrow("Unknown translation");
  expect(() => registerLanguageResources("test.languages", catalog)).toThrow("Duplicate");
  expect(() => registerLanguageResources("test.keys", { ...catalog, "en-US": {} })).toThrow(
    "keys differ",
  );
  expect(() =>
    registerLanguageResources("test.arguments", {
      ...catalog,
      "en-US": { hello: "Hello {other}" },
    }),
  ).toThrow("placeholders differ");
});

test("language switches every workspace, preserves preferences and survives reload", async ({
  page,
}) => {
  const errors: string[] = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.goto("/");
  page = await openSettingsWindow(page);
  await page.getByLabel("显示密度", { exact: true }).selectOption("comfortable");
  await page.getByLabel("语言", { exact: true }).selectOption("en-US");
  await expect(page.locator("html")).toHaveAttribute("lang", "en-US");
  await expect(page.locator(".environment-label")).toHaveText("Development");
  await expect(page.getByRole("heading", { name: "Preferences", exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Preferences", exact: true }).click();
  await expect(page.getByLabel("Display Density", { exact: true })).toHaveValue("comfortable");
  await page.screenshot({ path: "apps/clients/terminal/test-results/english-settings.png" });
  await page.getByRole("button", { name: "Connections & deployment", exact: true }).click();
  await expect(page.locator(".settings-content")).not.toContainText(/\p{Script=Han}/u);
  await page.getByRole("button", { name: "Machines", exact: true }).click();
  await page.getByText("Service Manager Program", { exact: true }).click();
  await page.getByRole("button", { name: "Check Program Updates", exact: true }).click();
  await expect(
    page.getByText("System installation is not managed in development", { exact: true }),
  ).toBeVisible();
  const denied = await page.request.post("/__asterion/api", {
    data: { version: 1, method: "node.agent.upgrade", params: { expected_digest: "0".repeat(64) } },
  });
  expect((await denied.json()).error).toBeTruthy();

  await page.getByRole("button", { name: "Plugins", exact: true }).click();
  await expect(page.getByText("Registered · Loaded on Demand", { exact: true })).toHaveCount(6);
  page = await closeSettingsWindow(page);
  for (const workspace of [
    "Watchlist",
    "Contract",
    "Market",
    "Data",
    "Backtest & Factors",
    "Trading",
  ]) {
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: workspace, exact: true })
      .click();
    await expect(
      page.locator(".workspace-tabs").getByRole("button", { name: workspace, exact: true }),
    ).toHaveAttribute("aria-current", "page");
    await expect(page.locator("body")).not.toContainText(/\p{Script=Han}/u);
  }
  for (const label of await page.locator(".activity-rail .nav-label").all()) {
    expect(
      await label.evaluate(
        element =>
          element.getBoundingClientRect().right <=
          element.closest("aside")!.getBoundingClientRect().right,
      ),
    ).toBe(true);
  }
  await page.screenshot({ path: "apps/clients/terminal/test-results/english-workbench.png" });
  await page.reload();
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "Watchlist", exact: true }),
  ).toBeVisible();
  page = await openSettingsWindow(page);
  await page.getByRole("button", { name: "Preferences", exact: true }).click();
  await expect(page.getByLabel("Display Density", { exact: true })).toHaveValue("comfortable");
  await page.getByRole("button", { name: "Preferences", exact: true }).click();
  await page.getByLabel("Language", { exact: true }).selectOption("zh-CN");
  await expect(page.getByRole("heading", { name: "偏好设置", exact: true })).toBeVisible();
  page = await closeSettingsWindow(page);
  await page.reload();
  await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
  await expect(
    page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }),
  ).toBeVisible();
  expect(errors).toEqual([]);
});

test.describe("startup language", () => {
  test.use({ enterWorkbench: false });
  test("English setup persists into workbench and localizes backend failures", async ({ page }) => {
    await page.route("**/__asterion/api", route =>
      route.fulfill({
        contentType: "application/json",
        body: JSON.stringify({ error: { code: "permission_denied", message: "原始诊断信息" } }),
      }),
    );
    await page.goto("/");
    await page.getByLabel("语言", { exact: true }).selectOption("en");
    await page.screenshot({ path: "apps/clients/terminal/test-results/english-setup.png" });
    await expect(page.getByRole("alert")).toContainText("Permission denied");
    await expect(page.getByRole("alert")).not.toContainText("原始诊断信息");
    await page.getByRole("alert").getByRole("button", { name: "Details", exact: true }).click();
    await expect(page.getByRole("alert")).toContainText("permission_denied: 原始诊断信息");
    await page.unroute("**/__asterion/api");
    // With every step passed and nothing to point out, the workbench opens.
    await page.getByRole("button", { name: "RETRY", exact: true }).click();
    await expect(
      page.locator(".workspace-tabs").getByRole("button", { name: "Watchlist", exact: true }),
    ).toBeVisible();
    await page.reload();
    await expect(
      page.locator(".workspace-tabs").getByRole("button", { name: "Watchlist", exact: true }),
    ).toBeVisible();
  });
});
