import { removeFolder } from "./cleanup";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

for (const mode of ["full", "holdout", "search", "rolling"])
  test(`Agent restores ${mode} factor results`, async ({ page }) => {
    const rolling = mode === "rolling",
      holdout = mode !== "full",
      search = mode === "search" || rolling;
    const folder = await mkdtemp(join(tmpdir(), "asterion-research-ui-"));
    const csv = join(folder, "research.csv");
    const start = 1790298000000000000n;
    await writeFile(
      csv,
      "timestamp_ns,price,quantity\n" +
        Array.from({ length: rolling ? 160 : holdout ? 100 : 40 }, (_, i) => 100 + i + (i % 3))
          .map((price, index) => `${start + BigInt(index) * 1000000000n},${price},1`)
          .join("\n") +
        "\n",
    );
    try {
      await page.goto("/");
      await page.getByRole("button", { name: "数据", exact: true }).click();
      if ((await page.locator("details.data-import").getAttribute("open")) === null)
        await page.getByText("导入 CSV", { exact: true }).click();
      for (const [label, value] of [
        ["CSV 文件路径", csv],
        ["品种代码", "rb"],
        ["实际合约", "rb2610"],
        ["交割月份", "2026-10"],
        ["价格步长", "1"],
        ["每手乘数", "10"],
      ])
        await page.getByLabel(label, { exact: true }).fill(value);
      await page.getByRole("button", { name: "校验并预览", exact: true }).click();
      await expect(page.getByText("文件校验完成，可在市场工作区查看历史成交。")).toBeVisible();
      await page.getByRole("button", { name: "研究", exact: true }).click();
      const research = page.getByRole("region", { name: "期货研究", exact: true });
      await expect(research.getByText("研究服务已连接", { exact: true })).toBeVisible();
      await research.getByRole("button", { name: "因子分析", exact: true }).click();
      await expect(research.getByLabel("初始资金", { exact: true })).toHaveCount(0);
      await research.getByLabel("回看笔数", { exact: true }).fill(search ? "2,5,10" : "2");
      await research.getByLabel("未来收益笔数", { exact: true }).fill("1");
      if (rolling) {
        await research.getByLabel("评价方式", { exact: true }).selectOption("walk_forward");
        await research.getByLabel("训练成交笔数", { exact: true }).fill("80");
        await research.getByLabel("每轮验证笔数", { exact: true }).fill("40");
      } else if (holdout) {
        await research.getByLabel("评价方式", { exact: true }).selectOption("holdout");
        await research.getByLabel("前段成交笔数", { exact: true }).fill("50");
      }
      const taskIds = () =>
        research
          .getByRole("region", { name: "研究任务", exact: true })
          .locator("tbody tr code")
          .allTextContents();
      const before = new Set(await taskIds());
      await research.getByRole("button", { name: "开始分析", exact: true }).click();
      await expect(research.getByText("任务已提交，可关闭窗口。", { exact: true })).toBeVisible();
      await expect.poll(async () => (await taskIds()).filter(id => !before.has(id)).length).toBe(1);
      const taskId = (await taskIds()).find(id => !before.has(id))!;
      await page.reload();
      await page.getByRole("button", { name: "研究", exact: true }).click();
      const row = research.getByRole("row").filter({ hasText: taskId });
      await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
      await row.getByRole("button", { name: "查看结果", exact: true }).click();
      await expect(research.getByRole("button", { name: "因子分析", exact: true })).toHaveAttribute(
        "aria-pressed",
        "true",
      );
      await expect(research.getByLabel("初始资金", { exact: true })).toHaveCount(0);
      const result = research.getByRole("region", { name: "因子结果", exact: true });
      if (rolling) {
        const folds = result.getByRole("table", { name: "滚动验证结果", exact: true });
        await expect(folds.locator("tbody tr")).toHaveCount(2);
        await expect(folds.locator("tbody tr").nth(0)).toContainText("1–80");
        await expect(folds.locator("tbody tr").nth(0)).toContainText("81–120");
        await expect(folds.locator("tbody tr").nth(1)).toContainText("41–120");
        await expect(folds.locator("tbody tr").nth(1)).toContainText("121–160");
      } else if (holdout) {
        await expect(
          result
            .getByRole("region", { name: "前段", exact: true })
            .getByText(search ? "39" : "47", { exact: true }),
        ).toBeVisible();
        await expect(
          result
            .getByRole("region", { name: "留出段", exact: true })
            .getByText("49", { exact: true }),
        ).toBeVisible();
      } else await expect(result.getByText("37", { exact: true })).toBeVisible();
      await research.getByLabel("回看笔数", { exact: true }).fill("30");
      await research.getByLabel("未来收益笔数", { exact: true }).fill("3");
      await result.getByText("实验参数", { exact: true }).click();
      const parameter = (label: string) =>
        result
          .locator(".experiment-fields > div")
          .filter({ has: page.getByText(label, { exact: true }) })
          .locator("dd");
      await expect(parameter("候选回看笔数")).toHaveText(search ? "2, 5, 10" : "2");
      await expect(parameter("未来收益笔数")).toHaveText("1");
      await expect(parameter("评价方式")).toHaveText(
        rolling ? "滚动验证" : holdout ? "时间留出评价" : "全样本评价",
      );
      if (rolling) {
        await expect(parameter("训练成交笔数")).toHaveText("80");
        await expect(parameter("每轮验证笔数")).toHaveText("40");
      } else if (holdout) await expect(parameter("前段成交笔数")).toHaveText("50");
      await result.getByText("结果详情", { exact: true }).click();
      await expect(result).toContainText(`输入成交: ${rolling ? 160 : holdout ? 100 : 40}`);
      await expect(result).toContainText(`剔除跨界标签: ${rolling ? 2 : holdout ? 1 : 0}`);
      await result.getByText("样本明细（前 100 条）", { exact: true }).click();
      const firstSample = result.getByRole("row").nth(1);
      if (rolling) {
        await result.getByText("逐轮选参证据", { exact: true }).click();
        await expect(result).toContainText("共同预热笔数: 10");
        await result.screenshot({ path: join(__dirname, "../test-results/factor-rolling.png") });
      } else if (search) {
        await result.getByText("候选比较", { exact: true }).click();
        await expect(result.getByText("39", { exact: true })).toHaveCount(4);
        await expect(result).toContainText("选中回看笔数");
        await expect(result).toContainText("共同预热笔数: 10");
      } else {
        await expect(firstSample).toContainText("4.0000%");
        await expect(firstSample).toContainText("-0.9615%");
      }
      await result
        .locator(".research-experiment")
        .screenshot({ path: join(__dirname, `../test-results/factor-${mode}-evidence.png`) });
      await page.screenshot({ path: join(__dirname, `../test-results/factor-${mode}-result.png`) });
      await page.getByRole("button", { name: "任务", exact: true }).click();
      await expect(page.getByRole("region", { name: "任务中心", exact: true })).toContainText(
        "SHFE/rb2610",
      );
      await page.getByRole("button", { name: "关闭任务中心" }).click();
      await page.setViewportSize({ width: 800, height: 900 });
      await expect(
        research.getByRole("button", { name: "查看结果", exact: true }).last(),
      ).toBeVisible();
      expect(
        await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth),
      ).toBe(true);
      if (rolling) {
        page = await openSettingsWindow(page);
        await page.getByLabel("语言", { exact: true }).selectOption("en-US");
        page = await closeSettingsWindow(page);
        await page.getByRole("button", { name: "Research", exact: true }).click();
        await expect(
          page.getByRole("button", { name: "Factor Analysis", exact: true }),
        ).toHaveAttribute("aria-pressed", "true");
        const english = page.getByRole("region", { name: "Factor Results", exact: true });
        await expect(
          english.getByRole("table", { name: "Walk-forward results", exact: true }),
        ).toBeVisible();
        await expect(english).not.toContainText(/\p{Script=Han}/u);
        await english.screenshot({
          path: join(__dirname, "../test-results/factor-rolling-english.png"),
        });
      }
    } finally {
      await removeFolder(folder);
    }
  });
