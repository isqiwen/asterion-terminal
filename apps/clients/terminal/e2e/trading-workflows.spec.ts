import { test, expect } from "./test";
import { seedDataset } from "./dataset-fixture";

for (const locale of ["zh-CN", "en-US"]) {
  test(`account and backtest wizards retain drafts and fit ${locale}`, async ({ page }) => {
    const en = locale === "en-US";
    const errors: string[] = [];
    page.on("pageerror", error => errors.push(error.message));
    await page.addInitScript(value => localStorage.setItem("asterion.locale", value), locale);
    await page.goto("/");
    await seedDataset(page.request, [100, 101, 102], `workflow-${locale}`);
    await page.reload();
    await page.setViewportSize({ width: 1024, height: 768 });
    const button = (zh: string, english: string) =>
      page.getByRole("button", { name: en ? english : zh, exact: true });
    const fits = async () =>
      expect(
        await page
          .locator(".terminal-business")
          .evaluate(el => el.scrollWidth <= el.clientWidth + 1),
      ).toBe(true);
    await button("交易", "Trading").click();
    await expect(
      page.getByRole("region", { name: en ? "CTP trading account" : "CTP 交易账户", exact: true }),
    ).toBeVisible();
    await fits();
    await page.screenshot({ path: `apps/clients/terminal/test-results/ctp-setup-${locale}.png` });
    await button("研究", "Research").click();
    await button("均线回测", "Moving Average Backtest").click();
    await button("新建回测", "New backtest").click();
    await button("下一步", "Next").click();
    await page.getByLabel(en ? "Initial Capital" : "初始资金", { exact: true }).fill("25000");
    await fits();
    // A draft survives leaving the workspace and coming back.
    await button("交易", "Trading").click();
    await button("研究", "Research").click();
    await expect(page.getByLabel(en ? "Initial Capital" : "初始资金", { exact: true })).toHaveValue(
      "25000",
    );
    await page.screenshot({
      path: `apps/clients/terminal/test-results/backtest-setup-${locale}.png`,
    });
    // Check visible product copy; hidden drafts can contain user-named datasets.
    if (en)
      expect(await page.locator(".terminal-business").innerText()).not.toMatch(/\p{Script=Han}/u);
    expect(errors).toEqual([]);
  });
}
