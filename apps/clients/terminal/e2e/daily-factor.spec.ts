import { test, expect } from "./test";
for (const locale of ["zh-CN", "en-US"]) {
  test(`daily factor empty state and retained parameters fit ${locale}`, async ({ page }) => {
    const en = locale === "en-US";
    await page.addInitScript(value => localStorage.setItem("asterion.locale", value), locale);
    await page.goto("/");
    await page
      .getByRole("button", { name: en ? "Backtest & Factors" : "回测与因子", exact: true })
      .click();
    await page.getByRole("button", { name: en ? "Daily factor" : "日线因子", exact: true }).click();
    await expect(
      page.getByRole("button", { name: en ? "Analyze daily bars" : "开始日线分析", exact: true }),
    ).toBeDisabled();
    const lookback = page.getByLabel(en ? "Lookback bars" : "回看日线数", { exact: true });
    await lookback.fill("17");
    await page.getByLabel(en ? "Evaluation" : "评价方式", { exact: true }).selectOption("holdout");
    await page.getByLabel(en ? "Development bars" : "前段日线数", { exact: true }).fill("60");
    await page
      .getByRole("button", { name: en ? "Factor Analysis" : "因子分析", exact: true })
      .click();
    await page.getByRole("button", { name: en ? "Daily factor" : "日线因子", exact: true }).click();
    await expect(lookback).toHaveValue("17");
    for (const width of [1440, 800]) {
      await page.setViewportSize({ width, height: 900 });
      expect(
        await page
          .locator(".backtest-factor-workspace")
          .evaluate(el => el.scrollWidth <= el.clientWidth + 1),
      ).toBe(true);
      await page.screenshot({
        path: `apps/clients/terminal/test-results/daily-factor-${locale}-${width}.png`,
      });
    }
    await page
      .getByRole("button", { name: en ? "Download daily history" : "下载历史日线", exact: true })
      .click();
    await expect(
      page.getByRole("region", { name: en ? "Historical data" : "历史数据", exact: true }),
    ).toBeVisible();
  });
}

test("download factor entry binds the selected source to its service", async ({ page }) => {
  let service = "source-service";
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!["runtime.snapshot", "factor.daily.submit", "data.datasets"].includes(request.method))
      return route.continue();
    const response = await route.fetch({
      postData: { ...request, method: "runtime.snapshot", params: {} },
    });
    const data = await response.json();
    data.result.data = { ...data.result.data, connection_id: `data-${service}` };
    data.result.task_service = {
      ...data.result.task_service,
      connection_id: service,
      host: "",
      online: true,
      remote: false,
      tasks: [
        {
          id: "same-id",
          history_dataset_id: "a".repeat(64),
          kind: "daily_download",
          data_source: "tushare.fut_daily",
          state: "succeeded",
          attempt: 1,
          instrument: "SHFE/cu/2024-03",
          source_name: "Explicit daily fixture",
          result_digest: "a".repeat(64),
          total: 1,
          completed: 1,
          submission_sequence: 1,
          submitted_at_ms: 1,
          updated_at_ms: 1,
          error: "",
          trading_day: "",
        },
      ],
    };
    if (request.method === "data.datasets")
      data.result.history_datasets = [
        {
          id: "a".repeat(64),
          revision: "a".repeat(64),
          contract_id: "SHFE/cu/2024-03",
          source: "tushare.fut_daily",
          begin: "2023-01-01",
          end: "2023-06-01",
          interval_minutes: 0,
          rows: 80,
        },
      ];
    await route.fulfill({ response, json: data });
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page
    .getByRole("combobox", { name: "数据源", exact: true })
    .selectOption("tushare.fut_daily");
  await page.getByRole("button", { name: "日线因子分析", exact: true }).click();
  await expect(page.getByLabel("日线来源", { exact: true })).toHaveValue("a".repeat(64));
  service = "other-service";
  await expect(
    page.getByText("数据连接已改变，请重新选择当前服务中的日线。", { exact: true }),
  ).toBeVisible();
  await expect(page.getByLabel("日线来源", { exact: true })).toHaveValue("");
  await expect(page.getByRole("button", { name: "开始日线分析", exact: true })).toBeDisabled();
  // The other service has the same task ID; only an explicit selection may use it.
  await page.getByLabel("日线来源", { exact: true }).selectOption("a".repeat(64));
  await expect(page.getByRole("button", { name: "开始日线分析", exact: true })).toBeEnabled();
  await page.getByRole("button", { name: "开始日线分析", exact: true }).click();
  const submitted = page.getByText("任务已提交，可在任务中心查看进度。", { exact: true });
  await expect(submitted).toBeVisible();
  service = "third-service";
  await expect(page.getByLabel("日线来源", { exact: true })).toHaveValue("");
  await expect(submitted).toHaveCount(0);
});
