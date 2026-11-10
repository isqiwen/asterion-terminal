import { openView } from "./navigation-helper";
import { test, expect } from "./test";

test("product navigation and header search replace duplicate workspace tabs without losing drafts", async ({
  page,
}) => {
  await page.goto("/");
  await openView(page, "market");
  const search = page.getByRole("searchbox", { name: "搜索市场合约" });
  await expect(search).toBeVisible();
  await expect(page.locator(".open-workspaces")).toHaveCount(0);
  await search.fill("rb26");
  await openView(page, "coverage");
  await expect(search).toHaveCount(0);
  await openView(page, "market");
  await expect(search).toHaveValue("rb26");
  await page.setViewportSize({ width: 640, height: 800 });
  await expect(search).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
