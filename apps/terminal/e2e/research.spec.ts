import { removeFolder } from "./cleanup";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

for (const variant of ["day session", "night sessions", "multiple days"])
  test(`Agent runs ${variant} backtest and restores results`, async ({ page }) => {
    const night = variant === "night sessions",
      multi = variant === "multiple days";
    const folder = await mkdtemp(join(tmpdir(), "asterion-research-ui-"));
    const csv = join(folder, "research.csv");
    const start = 1790298000000000000n;
    const prices = multi
      ? [100, 101, 102, 101, 104, 103, 102, 103]
      : [100, 101, 102, 101, 100, 101, 103];
    await writeFile(
      csv,
      "timestamp_ns,price,quantity\n" +
        prices
          .map(
            (price, index) =>
              `${multi ? start + (index < 4 ? 0n : 259200000000000n) + BigInt(index % 4) * 1000000000n : (night && index < 3 ? 1790254800000000000n : start) + BigInt(index) * 1000000000n},${price},1`,
          )
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
      await expect(research.getByLabel("交易日", { exact: true })).toHaveValue("");
      await research.getByLabel("交易日", { exact: true }).fill("2026-09-25");
      await research
        .getByLabel("开始时刻 1", { exact: true })
        .fill(night ? "2026-09-24T21:00" : "2026-09-25T09:00");
      await research
        .getByLabel("结束时刻 1", { exact: true })
        .fill(night ? "2026-09-24T21:00:03" : "2026-09-25T09:00:10");
      if (night) {
        await research.getByRole("button", { name: "添加时段", exact: true }).click();
        await research.getByLabel("开始时刻 2", { exact: true }).fill("2026-09-25T09:00");
        await research.getByLabel("结束时刻 2", { exact: true }).fill("2026-09-25T09:00:10");
      }
      await research.getByLabel("时段来源", { exact: true }).fill("test fixture");
      await research.getByLabel("结算价", { exact: true }).fill(multi ? "105" : "103");
      await research.getByLabel("结算价来源", { exact: true }).fill("test settlement");
      if (multi) {
        await research.getByRole("button", { name: "添加交易日", exact: true }).click();
        const second = research.getByRole("group", { name: "交易日 2", exact: true });
        await second.getByLabel("交易日", { exact: true }).fill("2026-09-28");
        await second.getByLabel("开始时刻 1", { exact: true }).fill("2026-09-28T09:00");
        await second.getByLabel("结束时刻 1", { exact: true }).fill("2026-09-28T09:00:10");
        await second.getByLabel("时段来源", { exact: true }).fill("second fixture");
        await second.getByLabel("结算价", { exact: true }).fill("110");
        await second.getByLabel("结算价来源", { exact: true }).fill("second settlement");
      }
      for (const [label, value] of [
        ["快均线", "1"],
        ["慢均线", "3"],
        ["目标手数", "1"],
        ["初始资金", "10000"],
        ["每手保证金", "100"],
        ["开仓手续费", "2"],
        ["平今手续费", "3"],
        ["平昨手续费", "4"],
        ["单笔数量上限", "100"],
        ["总持仓量上限", "100"],
        ["在途委托数上限", "100"],
      ])
        await research.getByLabel(label, { exact: true }).fill(value);
      if (night)
        await research
          .getByRole("group", { name: "交易时段（北京时间）", exact: true })
          .screenshot({ path: join(__dirname, "../test-results/research-night-sessions.png") });
      const taskIds = () =>
        research
          .getByRole("region", { name: "研究任务", exact: true })
          .locator("tbody tr code")
          .allTextContents();
      const before = new Set(await taskIds());
      await research.getByRole("button", { name: "开始回测", exact: true }).click();
      await expect(research.getByText("任务已提交，可关闭窗口。", { exact: true })).toBeVisible();
      await expect.poll(async () => (await taskIds()).filter(id => !before.has(id)).length).toBe(1);
      const taskId = (await taskIds()).find(id => !before.has(id))!;
      await page.reload();
      await page.getByRole("button", { name: "研究", exact: true }).click();
      const row = research
        .getByRole("region", { name: "研究任务", exact: true })
        .getByRole("row")
        .filter({ hasText: taskId });
      await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
      await research.getByLabel("快均线", { exact: true }).fill("9");
      await research.getByLabel("初始资金", { exact: true }).fill("50000");
      await row.getByRole("button", { name: "查看结果", exact: true }).click();
      const result = research.getByRole("region", { name: "回测结果", exact: true });
      const metric = (label: string) =>
        result
          .locator(".research-metrics > div")
          .filter({ has: page.getByText(label, { exact: true }) })
          .locator("strong");
      await expect(metric("期末权益")).toHaveText(multi ? "10014" : night ? "10000" : "9995");
      await expect(metric("最大回撤金额")).toHaveText(multi ? "30" : night ? "0" : "12");
      await expect(result.getByRole("img", { name: "权益曲线" })).toBeVisible();
      await result.getByText("实验参数", { exact: true }).click();
      const parameter = (label: string) =>
        result
          .locator(".experiment-fields > div")
          .filter({ has: page.getByText(label, { exact: true }) })
          .locator("dd");
      await expect(parameter("快均线")).toHaveText("1");
      await expect(parameter("初始资金")).toHaveText("10000");
      await expect(parameter("平今手续费")).toHaveText("3");
      await expect(parameter("单笔数量上限")).toHaveText("100");
      await expect(parameter("输入成交")).toHaveText(multi ? "8" : "7");
      await expect(parameter("时段来源").first()).toHaveText("test fixture");
      await expect(parameter("交易时段 1").first()).toContainText(
        night ? "2026/9/24" : "2026/9/25",
      );
      await expect(parameter("结算价").first()).toHaveText(multi ? "105" : "103");
      await result.getByText("逐日结算", { exact: true }).click();
      const settlements = result.locator(".research-settlements tbody tr");
      await expect(settlements).toHaveCount(multi ? 2 : 1);
      if (multi) {
        await expect(settlements.first()).toContainText("10038");
        await expect(settlements.last()).toContainText("10014");
      }
      if (night) await expect(parameter("交易时段 2")).toContainText("2026/9/25");
      await expect(research.getByLabel("快均线", { exact: true })).toHaveValue("9");
      await expect(research.getByLabel("初始资金", { exact: true })).toHaveValue("50000");
      await result.locator(".research-experiment").screenshot({
        path: join(
          __dirname,
          `../test-results/research-${multi ? "multi" : night ? "night" : "day"}-evidence.png`,
        ),
      });
      await page.screenshot({ path: join(__dirname, "../test-results/research-result.png") });
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
      if (night || multi) {
        page = await openSettingsWindow(page);
        await page.getByLabel("语言", { exact: true }).selectOption("en-US");
        page = await closeSettingsWindow(page);
        await page.getByRole("button", { name: "Research", exact: true }).click();
        const english = page.getByRole("region", { name: "Backtest Result", exact: true });
        // The parameters <details> opened in Chinese stays open across the
        // language switch; clicking it again would collapse it.
        if ((await english.locator("details.research-experiment").getAttribute("open")) === null)
          await english.getByText("Experiment Parameters", { exact: true }).click();
        await expect(english).not.toContainText(/\p{Script=Han}/u);
        if (night)
          await expect(english.getByText("Trading session 2", { exact: true })).toBeVisible();
        if (multi)
          await expect(
            english.getByText("Trading Day · 2026-09-28", { exact: true }),
          ).toBeVisible();
        await english
          .locator(".research-experiment")
          .screenshot({ path: join(__dirname, "../test-results/research-night-english.png") });
      }
    } finally {
      await removeFolder(folder);
    }
  });
