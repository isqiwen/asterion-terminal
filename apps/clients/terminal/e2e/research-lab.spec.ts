import { execFileSync } from "node:child_process";
import { cpSync, existsSync } from "node:fs";
import { join } from "node:path";
import { rpc } from "./dataset-fixture";
import { test, expect, type Page } from "./test";

const source = join(__dirname, "../../../..");

// The example strategies in a git repository, and lab's fixture warehouse in
// the isolated root's lab directory.
function prepare(root: string) {
  const repository = join(root, "strategies");
  // The tests of this file share one root.
  if (existsSync(repository)) return repository;
  cpSync(join(source, "apps/services/lab/examples"), repository, { recursive: true });
  const git = (...args: string[]) =>
    execFileSync("git", [
      "-C",
      repository,
      "-c",
      "user.name=test",
      "-c",
      "user.email=test@invalid",
      ...args,
    ]);
  git("init", "-q");
  git("add", ".");
  git("commit", "-qm", "examples");
  execFileSync(join(source, "build/electron-resources/python/bin/python3"), [
    "-I",
    "-B",
    join(source, "tests/lab/warehouse_fixture.py"),
    join(source, "apps/services/lab"),
    join(root, "lab/data/lab"),
  ]);
  return repository;
}

async function backtest(page: Page, fast: string, slow: string) {
  const form = page.locator(".lab-research");
  await form.getByLabel("fast").fill(fast);
  await form.getByLabel("slow").fill(slow);
  await form.getByRole("button", { name: "运行", exact: true }).click();
  await expect(page.getByRole("heading", { name: "月度收益" })).toBeVisible({ timeout: 30000 });
}

test(
  "a strategy from the repository is configured, run, read and compared",
  { tag: "@journey" },
  async ({ page }) => {
    const repository = prepare(process.env.ASTERION_ROOT!);
    await rpc(page.request, "research.repository.save", { path: repository });
    await page.goto("/");
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page.getByRole("button", { name: /MaCross/ }).click();
    const form = page.locator(".lab-research");
    await form.getByLabel("品种").fill("SHFE/rb");
    await form.getByLabel("开始日").fill("2026-01-05");
    await form.getByLabel("结束日").fill("2026-02-13");
    await form.getByLabel("单笔手数").fill("10");
    await form.getByLabel("总持仓手数").fill("10");
    await form.getByText("SHFE/rb 的费用").click();
    for (const [label, value] of [
      ["每手保证金", "0"],
      ["开仓手续费", "3"],
      ["平今手续费", "3"],
      ["平昨手续费", "3"],
      ["保证金率", "0.1"],
      ["开仓手续费率", "0"],
      ["平今手续费率", "0"],
      ["平昨手续费率", "0"],
    ])
      await form.getByLabel(label, { exact: true }).fill(value);

    await backtest(page, "2", "4");
    await expect(page.getByText(/^成交（[1-9]\d* 笔）$/)).toBeVisible();
    await expect(page.getByText("K 线与买卖点")).toBeVisible();
    await page.screenshot({ path: join(__dirname, "../test-results/research-lab-result.png") });

    await backtest(page, "3", "6");
    const runs = page.locator(".lab-runs tbody tr");
    await expect(runs).toHaveCount(2);
    await expect(runs.locator(".lab-state")).toHaveText(["已完成", "已完成"]);
    for (const box of await page.getByRole("checkbox", { name: /^比较 / }).all()) await box.check();
    await page.getByRole("button", { name: "比较所选两次" }).click();
    const comparison = page.locator(".lab-center table");
    await expect(comparison.getByRole("columnheader", { name: /^ma_cross-\d+$/ })).toHaveCount(2);
    await expect(comparison.getByRole("row", { name: /^总收益 / })).toBeVisible();
    await page.screenshot({ path: join(__dirname, "../test-results/research-lab-comparison.png") });
  },
);

test(
  "a factor from the repository is evaluated across symbols",
  { tag: "@journey" },
  async ({ page }) => {
    const repository = prepare(process.env.ASTERION_ROOT!);
    await rpc(page.request, "research.repository.save", { path: repository });
    await page.goto("/");
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page.getByRole("button", { name: /^momentum price · close/ }).click();
    const form = page.locator(".lab-config");
    await form.getByLabel("品种").fill("SHFE/rb/2026-05, SHFE/rb/2026-10, SHFE/rb");
    await form.getByLabel("开始日").fill("2026-01-05");
    await form.getByLabel("结束日").fill("2026-02-13");
    await form.getByLabel("lookback").fill("2");
    await form.getByLabel("分组数").fill("3");
    await form.getByRole("button", { name: "评价", exact: true }).click();
    const decay = page.getByRole("table", { name: "IC 与衰减" });
    await expect(decay.locator("tbody tr")).toHaveCount(6);
    await expect(decay.locator("tbody tr").first()).toContainText("1 根");
    await expect(page.getByRole("table", { name: "分组" }).locator("tbody tr")).toHaveCount(3);
    await expect(page.getByText("3 个品种")).toBeVisible();
    await page.screenshot({ path: join(__dirname, "../test-results/research-lab-factor.png") });
  },
);
