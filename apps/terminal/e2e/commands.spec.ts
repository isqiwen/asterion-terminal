import { expect, test } from "@playwright/test";

test("navigation buttons and shortcuts use the same command path", async ({
  page,
}) => {
  await page.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });
 return route.fulfill({ json: [] }); });
  await page.goto("/");
  const navigation = page.getByRole("navigation", { name: "业务工作区" });
  await navigation.getByRole("button", { name: "数据", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "数据", exact: true }),
  ).toBeVisible();
  await page.keyboard.press("Control+1");
  await expect(
    page.getByRole("heading", { name: "总览", exact: true }),
  ).toBeVisible();
  await page.keyboard.press("Control+3");
  await expect(
    navigation.getByRole("button", { name: "数据", exact: true }),
  ).toHaveAttribute("aria-current", "page");
  await page.keyboard.press("Control+j");
  await expect(page.getByRole("region", { name: "任务中心" })).toBeVisible();
  await page.getByRole("button", { name: "任务", exact: true }).click();
  await expect(page.getByRole("region", { name: "任务中心" })).toHaveCount(0);
});
