import { expect, test } from "@playwright/test";
test("settings open separately and apply preferences without changing the workspace", async ({
  page,
}) => {
  await page.goto("/");
  const tabs = page.getByRole("navigation", { name: "业务工作区" });
  await expect(tabs.getByRole("button")).toHaveCount(6);
  await expect(tabs).not.toContainText("设置");
  await expect(page.getByText("开发里程碑")).toHaveCount(0);
  await expect(page.getByRole("button", { name: "停止后台服务" })).toHaveCount(
    0,
  );
  await tabs.getByRole("button", { name: "研究", exact: true }).click();
  const popupPromise = page.waitForEvent("popup");
  await page.getByRole("button", { name: "设置", exact: true }).click();
  const settings = await popupPromise;
  await expect(
    settings.getByRole("heading", { name: "外观", exact: true }),
  ).toBeVisible();
  await expect(
    settings.getByRole("navigation", { name: "业务工作区" }),
  ).toHaveCount(0);
  await settings.getByLabel("信息密度").selectOption("comfortable");
  await expect(page.locator("html")).toHaveAttribute(
    "data-density",
    "comfortable",
  );
  await expect(
    page.getByRole("heading", { name: "研究", exact: true }),
  ).toBeVisible();
  await settings.getByRole("button", { name: "本机服务", exact: true }).click();
  await expect(settings.getByText("数据目录")).toBeVisible();
  await settings.close();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollHeight <= innerHeight,
    ),
  ).toBe(true);
  await page.getByRole("button", { name: "任务", exact: true }).click();
  await page.getByRole("button", { name: "收起任务中心" }).click();
  await expect(page.getByRole("region", { name: "任务中心" })).toHaveCount(0);
  await page.reload();
  await expect(
    page.getByRole("heading", { name: "研究", exact: true }),
  ).toBeVisible();
  await expect(page.getByRole("region", { name: "任务中心" })).toHaveCount(0);
  await page.screenshot({ path: "../../.state/workspace-layout.png" });
});
