import { test, expect } from "./test";

test("product navigation and header search replace duplicate workspace tabs without losing drafts", async ({
  page,
}) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  const search = page.getByRole("searchbox", { name: "搜索市场合约" });
  const modes = page.getByRole("tablist", { name: "行情来源" });
  await expect(search).toBeVisible();
  await expect(page.locator(".open-workspaces")).toHaveCount(0);
  const searchBox = await search.boundingBox();
  const modeBox = await modes.boundingBox();
  expect(searchBox!.y + searchBox!.height).toBeLessThanOrEqual(modeBox!.y);
  await search.fill("rb26");
  await page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }).click();
  await expect(search).toHaveCount(0);
  await page.locator(".workspace-tabs").getByRole("button", { name: "市场", exact: true }).click();
  await expect(search).toHaveValue("rb26");
  await page.getByRole("tab", { name: "历史行情", exact: true }).click();
  await expect(page.getByRole("tab", { name: "历史行情", exact: true })).toHaveAttribute(
    "aria-selected",
    "true",
  );
  await search.fill("cu26");
  await expect(page.getByRole("tab", { name: "实时行情", exact: true })).toHaveAttribute(
    "aria-selected",
    "true",
  );
  await page.setViewportSize({ width: 640, height: 800 });
  await expect(search).toBeVisible();
  await expect(page.getByRole("tab", { name: "历史行情", exact: true })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
