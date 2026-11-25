import { execFileSync } from "node:child_process";
import { join } from "node:path";
import { openView } from "./navigation-helper";
import { test, expect } from "./test";

const source = join(__dirname, "../../../..");

test(
  "the data page draws a contract's stored Warehouse days through Lab",
  { tag: "@journey" },
  async ({ page }) => {
    test.setTimeout(45000);
    execFileSync(join(source, "build/electron-resources/python/bin/python3"), [
      "-I",
      "-B",
      join(source, "tests/lab/warehouse_fixture.py"),
      join(source, "apps/services/lab"),
      join(process.env.ASTERION_TEST_WAREHOUSE_TLS!, "../../data"),
      "--fundamentals",
    ]);
    await page.goto("/");
    await openView(page, "dataset");
    // Includes Lab startup and the first remote cache download.
    const search = page.getByRole("searchbox", { name: "搜索品种或合约" });
    await expect(search).toBeVisible({ timeout: 30000 });
    await search.fill("rb2605");
    await page.getByRole("button", { name: "查看 rb2605" }).click();
    const chart = page.getByRole("region", { name: "合约历史 K 线" });
    // Without minutes the chart opens on days; minute periods are not offered.
    await expect(chart.getByRole("button", { name: "日K" })).toHaveAttribute(
      "aria-pressed",
      "true",
      { timeout: 30000 },
    );
    await expect(chart.getByRole("button", { name: "1分", exact: true })).toBeDisabled();
    await expect(chart.getByText("1–30 / 30")).toBeVisible();
    await chart.getByRole("button", { name: "周线" }).click();
    await expect(chart.getByText("1–6 / 6")).toBeVisible();
    await openView(page, "browse");
    for (const [name, value] of [
      ["仓单", "353.123456"],
      ["会员持仓", "测试会员"],
      ["库存周报", "428"],
    ]) {
      await page
        .getByRole("group", { name: "基本面", exact: true })
        .getByRole("button", { name, exact: true })
        .click();
      const report = page.getByRole("table", { name, exact: true });
      await expect(report.getByRole("cell", { name: value, exact: true })).toBeVisible();
      await expect(
        report.getByRole("columnheader", { name: "发布时间（北京时间）" }),
      ).toBeAttached();
    }
  },
);
