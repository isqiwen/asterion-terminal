import { seedTickFixture } from "./dataset-fixture";
import { removeFolder } from "./cleanup";
import { test, expect } from "@playwright/test";
import { mkdtemp, writeFile, unlink } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("published data remains usable for research after source deletion", async ({ page }) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-published-ui-"));
  const csv = join(folder, "published.csv");
  const start = 1790298000000000000n;
  await writeFile(
    csv,
    "timestamp_ns,price,quantity\n" +
      Array.from(
        { length: 40 },
        (_, i) => `${start + BigInt(i) * 1000000000n},${100 + i + (i % 3)},1`,
      ).join("\n") +
      "\n",
  );
  try {
    await page.goto("/");
    await page.getByRole("button", { name: "数据", exact: true }).click();
    await page.getByRole("button", { name: "数据存档与结算表", exact: true }).click();
    await seedTickFixture(page, csv, true);
    await unlink(csv);
    await page.reload();
    await page.getByRole("button", { name: "数据", exact: true }).click();
    await page.getByRole("button", { name: "数据存档与结算表", exact: true }).click();
    const publications = page.getByRole("region", { name: "数据发布", exact: true });
    await expect(publications.getByText("published.csv", { exact: true })).toBeVisible();
    await expect(publications.getByText("已发布", { exact: true })).toBeVisible({ timeout: 20000 });
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await expect(
      page.getByText("当前模型使用成交事件；日线请使用日线因子，分钟 K 线尚不支持研究。", {
        exact: true,
      }),
    ).toBeVisible();
    await page.getByRole("button", { name: "选择已发布成交数据", exact: true }).click();
    await expect(publications).toBeVisible();
    await expect(page.getByLabel("CSV 文件路径", { exact: true })).toHaveCount(0);
    await publications.getByRole("button", { name: "使用此版本", exact: true }).click();
    await expect(page.getByText("已发布数据", { exact: true })).toBeVisible();
    await expect(page.getByRole("button", { name: "发布数据版本", exact: true })).toHaveCount(0);
    await page.screenshot({
      path: join(__dirname, "../test-results/data-publication.png"),
      fullPage: true,
    });
    await page.setViewportSize({ width: 800, height: 900 });
    expect(
      await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth),
    ).toBe(true);
    await publications.getByRole("button", { name: "使用并开始研究", exact: true }).click();
    const research = page.getByRole("region", { name: "期货研究", exact: true });
    await research.getByRole("button", { name: "因子分析", exact: true }).click();
    await research.getByLabel("回看笔数", { exact: true }).fill("2");
    await research.getByLabel("未来收益笔数", { exact: true }).fill("1");
    // Commit the request in C++, then lose only its response. Navigation must not
    // turn manual confirmation into a second task with a new identity.
    const submissions: string[] = [];
    await page.route("**/__asterion/api", async route => {
      const request = route.request().postDataJSON();
      if (request.method !== "research.factor.submit") return route.continue();
      submissions.push(request.params.id);
      if (submissions.length === 1) {
        const response = await route.fetch();
        expect((await response.json()).error).toBeUndefined();
        return route.fulfill({ status: 503, body: "Response lost by test fixture" });
      }
      return route.continue();
    });
    await research.getByRole("button", { name: "开始分析", exact: true }).click();
    await expect(research.getByRole("button", { name: "确认提交状态", exact: true })).toBeEnabled();
    await page.getByRole("button", { name: "数据", exact: true }).click();
    await page.getByRole("button", { name: "数据存档与结算表", exact: true }).click();
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await expect(research.getByLabel("回看笔数", { exact: true })).toHaveValue("2");
    await research.getByRole("button", { name: "确认提交状态", exact: true }).click();
    await expect(research.getByText("任务已提交，可关闭窗口。", { exact: true })).toBeVisible();
    expect(submissions).toHaveLength(2);
    expect(submissions[1]).toBe(submissions[0]);
    await expect(research.getByRole("row").filter({ hasText: submissions[0] })).toHaveCount(1);

    const row = research.getByRole("row").filter({ hasText: "动量因子" }).last();
    await expect(row.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
    await row.getByRole("button", { name: "查看结果", exact: true }).click();
    await expect(
      research
        .getByRole("region", { name: "因子结果", exact: true })
        .getByText("37", { exact: true }),
    ).toBeVisible();
    await expect(research.getByText("发布中", { exact: true })).toHaveCount(0);
  } finally {
    await removeFolder(folder);
  }
});
