import { execFileSync } from "node:child_process";
import { readFileSync, rmSync } from "node:fs";
import { join, resolve } from "node:path";
import { openView } from "./navigation-helper";
import { rpc } from "./rpc-fixture";
import { test, expect } from "./test";

// Only the independently supervised test Market gets this fixture. Lab's
// directory stays absent: data browsing must work without starting Python.
test(
  "Market data page reads the shared warehouse with read-only permissions and no live watch",
  { tag: "@journey" },
  async ({ page }) => {
    const source = resolve(__dirname, "../../../..");
    const host = resolve(process.env.ASTERION_TEST_MARKET_TLS!, "../..");
    const data = join(host, "data");
    execFileSync(join(source, "build/electron-resources/python/bin/python3"), [
      "-I",
      "-B",
      join(source, "tests/lab/warehouse_fixture.py"),
      join(source, "apps/services/lab"),
      data,
    ]);
    try {
      const version = readFileSync(join(data, "sources/research/warehouse/current"), "utf8").trim();
      await page.goto("/");
      await openView(page, "coverage");
      await rpc(page.request, "market.close");
      await page.getByRole("combobox", { name: "数据源", exact: true }).selectOption("shfe");
      await expect(
        page.getByText("当前连接只有读取权限，不能启动或取消采集。", { exact: true }),
      ).toBeVisible();
      await expect(page.getByRole("button", { name: "开始采集", exact: true })).toBeDisabled();
      await page.getByRole("combobox", { name: "数据源", exact: true }).selectOption("research");
      await expect(
        page.getByRole("region", { name: "数据集版本" }).locator(`code[title="${version}"]`),
      ).toBeVisible();
      await expect(page.getByRole("rowheader", { name: "SHFE/rb", exact: true })).toBeVisible();
      await expect(page.getByRole("columnheader", { name: "2026-01", exact: true })).toBeVisible();
      await expect(page.getByRole("button", { name: "开始采集", exact: true })).toHaveCount(0);
      expect((await rpc(page.request, "market.watch", { restart: false })).market_watch).toBeNull();
    } finally {
      rmSync(join(data, "sources/research"), { recursive: true, force: true });
    }
  },
);
