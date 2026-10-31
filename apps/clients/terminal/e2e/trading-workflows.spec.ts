import { openView } from "./navigation-helper";
import { test, expect } from "./test";

for (const locale of ["zh-CN", "en-US"]) {
  test(`the account and research pages fit ${locale}`, async ({ page }) => {
    const en = locale === "en-US";
    const errors: string[] = [];
    page.on("pageerror", error => errors.push(error.message));
    await page.addInitScript(value => localStorage.setItem("asterion.locale", value), locale);
    await page.goto("/");
    await expect(page.locator(".workspace-tabs")).toBeVisible();
    await page.setViewportSize({ width: 1024, height: 768 });
    const fits = async () =>
      expect(
        await page
          .locator(".terminal-business")
          .evaluate(el => el.scrollWidth <= el.clientWidth + 1),
      ).toBe(true);
    await openView(page, "trading");
    await expect(
      page.getByRole("region", { name: en ? "CTP trading account" : "CTP 交易账户", exact: true }),
    ).toBeVisible();
    await fits();
    await page.screenshot({ path: `apps/clients/terminal/test-results/ctp-setup-${locale}.png` });
    await openView(page, "research");
    await expect(
      page.getByRole("heading", { name: en ? "Research" : "研究", exact: true }),
    ).toBeVisible();
    await fits();
    // Check visible product copy in English.
    if (en)
      expect(await page.locator(".terminal-business").innerText()).not.toMatch(/\p{Script=Han}/u);
    expect(errors).toEqual([]);
  });
}
