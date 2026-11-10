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
    ]);
    await page.goto("/");
    await openView(page, "coverage");
    await page
      .getByRole("navigation", { name: "表" })
      .getByRole("button", { name: "已采集合约" })
      .click();
    const contracts = page.getByRole("group", { name: "已采集合约" });
    // Includes Lab startup and the first remote cache download.
    await expect(contracts.getByRole("button")).toHaveCount(2, { timeout: 30000 });
    await expect(contracts.getByRole("button").first()).toContainText("日线 30 天");
    const chart = page.getByRole("region", { name: "合约历史 K 线" });
    // Without minutes the chart opens on days; minute periods are not offered.
    await expect(chart.getByRole("button", { name: "日K" })).toHaveAttribute(
      "aria-pressed",
      "true",
    );
    await expect(chart.getByRole("button", { name: "1 min", exact: true })).toBeDisabled();
    await expect(chart.getByText("1–30 / 30")).toBeVisible();
    await chart.getByRole("button", { name: "周线" }).click();
    await expect(chart.getByText("1–6 / 6")).toBeVisible();
  },
);
