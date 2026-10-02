import { test, expect, type Page } from "./test";
import type { Snapshot } from "../src/bridge/client";

const catalogFixture = {
  source: "tushare.ft_mins",
  exchange: "SHFE",
  product: "CU",
  cutoff_ns: "1790582400000000000",
  items: [
    {
      code: "SHFE/cu/2023-10",
      name: "Copper 2310",
      list_date: "20221017",
      delist_date: "20231016",
      multiplier: null,
      per_unit: "5.00000001",
      trade_unit: "吨",
      quote_unit: "元/吨",
    },
  ],
};
async function mockCatalog(page: Page, identity?: { connection?: string }) {
  let loaded = false;
  let source = "tushare.ft_mins";
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!["research.contracts.load", "runtime.snapshot"].includes(request.method))
      return route.continue();
    if (request.method === "research.contracts.load") {
      expect(request.params.exchange).toBe("SHFE");
      expect(request.params.product).toBe("CU");
      loaded = true;
      source = request.params.source;
    }
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    if (loaded) data.result.history_contracts = { ...catalogFixture, source };
    if (identity?.connection && data.result.research)
      data.result.research.connection_id = identity.connection;
    await route.fulfill({ response, json: data });
  });
}

test("minute download keeps query drafts but never tokens and confirms uncertain submission", async ({
  page,
}) => {
  const identity: { connection?: string } = {};
  await mockCatalog(page, identity);
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  const section = page.getByRole("region", { name: "历史数据", exact: true });
  await section.getByLabel("品种代码", { exact: true }).fill("CU");
  await section.getByLabel("Tushare Token", { exact: true }).fill("ui-fixture-secret");
  await section.getByRole("button", { name: "查询月份合约", exact: true }).click();
  await section.getByLabel("月份合约", { exact: true }).selectOption("SHFE/cu/2023-10");
  await expect(section.locator('input[type="datetime-local"]')).toHaveCount(0);
  await expect(section.getByText(/20221017/)).toBeVisible();
  await section.getByText("合约计量信息", { exact: true }).click();
  const units = section
    .locator("details")
    .filter({ has: page.getByText("合约计量信息", { exact: true }) });
  await expect(units).toContainText("5.00000001");
  await expect(units).toContainText("元/吨");
  await expect(
    units
      .locator("div")
      .filter({ has: page.locator("dt", { hasText: "供应商合约乘数" }) })
      .locator("dd"),
  ).toHaveText("—");
  await units.scrollIntoViewIfNeeded();
  await page.screenshot({ path: "build/contract-units-browser.png" });
  await section.getByLabel("分钟周期", { exact: true }).selectOption("5");
  await section.getByLabel("Tushare Token", { exact: true }).fill("ui-fixture-secret");
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByRole("region", { name: "期货研究", exact: true })).toBeVisible();
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await expect(section.getByLabel("月份合约", { exact: true })).toHaveValue("SHFE/cu/2023-10");
  await expect(section.getByLabel("分钟周期", { exact: true })).toHaveValue("5");
  await expect(section.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
  const submissions: string[] = [];
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method !== "research.minutes.submit") return route.fallback();
    expect(request.params.contract_id).toBe("SHFE/cu/2023-10");
    expect(request.params.interval_minutes).toBe(5);
    expect(request.params).not.toHaveProperty("start");
    expect(request.params).not.toHaveProperty("end");
    expect(request.params.catalog_cutoff_ns).toBe(catalogFixture.cutoff_ns);
    expect(request.params.token).toBe("ui-fixture-secret");
    submissions.push(request.params.id);
    if (submissions.length === 1 || submissions.length === 3)
      return route.fulfill({ status: 503, body: "test response lost" });
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    data.result.history_contracts = catalogFixture;
    if (identity.connection) data.result.research!.connection_id = identity.connection;
    data.result.research!.tasks.push({
      kind: "minute_download",
      data_source: "tushare.ft_mins",
      id: request.params.id,
      state: "queued",
      attempt: 0,
      completed: 0,
      total: 2,
      error: "",
      result_digest: "",
      trading_day: "",
      instrument: "SHFE/cu/2023-10",
      source_name: "Tushare SHFE/cu/2023-10",
      submission_sequence: 100,
      submitted_at_ms: Date.now(),
      updated_at_ms: Date.now(),
    });
    return route.fulfill({ response, json: data });
  });
  await section.getByLabel("Tushare Token", { exact: true }).fill("ui-fixture-secret");
  await section.getByRole("button", { name: "下载整个合约", exact: true }).click();
  await expect(section.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
  await page.locator(".workspace-tabs").getByRole("button", { name: "自选", exact: true }).click();
  await expect(section).toHaveCount(0);
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await section.getByLabel("Tushare Token", { exact: true }).fill("ui-fixture-secret");
  await section.getByRole("button", { name: "确认下载提交", exact: true }).click();
  await expect(section.getByText("Tushare SHFE/cu/2023-10", { exact: true })).toBeVisible();
  await expect(section.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
  expect(submissions).toHaveLength(2);
  expect(submissions[1]).toBe(submissions[0]);
  await section.getByLabel("Tushare Token", { exact: true }).fill("ui-fixture-secret");
  await section.getByRole("button", { name: "下载整个合约", exact: true }).click();
  await expect(section.getByRole("button", { name: "确认下载提交", exact: true })).toBeVisible();
  identity.connection = "new-connection-same-host-and-service";
  await expect(section.getByRole("button", { name: "下载整个合约", exact: true })).toBeVisible();
  await expect(section.getByLabel("月份合约", { exact: true })).toHaveValue("SHFE/cu/2023-10");
  await expect(section.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
  await section.getByLabel("Tushare Token", { exact: true }).fill("ui-fixture-secret");
  await section.getByRole("button", { name: "下载整个合约", exact: true }).click();
  await expect.poll(() => submissions.length).toBe(4);
  expect(submissions[3]).not.toBe(submissions[2]);
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "ui-fixture-secret",
  );
  expect(await page.locator("body").innerText()).not.toContain("ui-fixture-secret");
  await page.setViewportSize({ width: 800, height: 900 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test("history is a dedicated source-aware page and clears credentials on subpage navigation", async ({
  page,
}) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  const history = page.getByRole("region", { name: "历史数据", exact: true });
  await expect(history).toBeVisible();
  await expect(page.getByLabel("CSV 文件路径")).toHaveCount(0);
  await expect(history.getByLabel("数据源", { exact: true }).locator("option")).toHaveText([
    "Tushare · 分钟 K 线",
    "Tushare · 日 K 线",
  ]);
  await expect(history.getByLabel("分钟周期", { exact: true }).locator("option")).toHaveText([
    "1 min",
    "5 min",
    "15 min",
    "30 min",
    "60 min",
  ]);
  await history.getByLabel("品种代码", { exact: true }).fill("CU");
  await history.getByLabel("Tushare Token", { exact: true }).fill("subpage-fixture-secret");
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  await expect(history).toHaveCount(0);
  await expect(page.getByLabel("CSV 文件路径")).toHaveCount(0);
  await page.getByRole("button", { name: "历史数据", exact: true }).click();
  await expect(history.getByLabel("品种代码", { exact: true })).toHaveValue("CU");
  await expect(history.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
  await page.screenshot({ path: "build/history-page-desktop.png", fullPage: true });
  await page.setViewportSize({ width: 800, height: 900 });
  await expect(history.locator(".history-layout")).toHaveCSS("grid-template-columns", /px$/);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: "build/history-page-compact.png", fullPage: true });
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  await expect(page.getByLabel("CSV 文件路径")).toHaveCount(0);
});

test("completed dataset exposes provenance and honest coverage separately from active tasks", async ({
  page,
}) => {
  const task = {
    kind: "minute_download" as const,
    data_source: "tushare.ft_mins",
    id: "history-dataset-fixture",
    state: "succeeded" as const,
    attempt: 1,
    completed: 1,
    total: 1,
    error: "",
    result_digest: "a".repeat(64),
    trading_day: "",
    instrument: "SHFE/cu/2023-10",
    source_name: "Tushare SHFE/cu/2023-10",
    submission_sequence: 100,
    submitted_at_ms: Date.now(),
    updated_at_ms: Date.now(),
  };
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!["runtime.snapshot", "research.result"].includes(request.method)) return route.continue();
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    if (!data.result.research) return route.fulfill({ response });
    data.result.research.tasks = [task];
    if (request.method === "research.result") {
      expect(request.params.id).toBe(task.id);
      data.result.research_result = {
        id: task.id,
        kind: "minute_download",
        task,
        experiment: {
          contract_id: "SHFE/cu/2023-10",
          interval_minutes: 5,
          begin_ns: "1692925200000000000",
          end_ns: "1692946800000000000",
        },
        result: {
          rows: 0,
          pages: 1,
          directory: "/fixture/history",
          manifest_sha256: "a".repeat(64),
        },
      };
    }
    await route.fulfill({ response, json: data });
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  const history = page.getByRole("region", { name: "历史数据", exact: true });
  await expect(history.getByText("暂无进行中的下载", { exact: true })).toBeVisible();
  await history.getByRole("button", { name: "数据集详情", exact: true }).click();
  await expect(
    history.getByText("结构与摘要已校验；交易时段覆盖未校验", { exact: false }),
  ).toBeVisible();
  await expect(
    history.getByText("请求区间内没有返回数据，请核对合约和时间范围。", { exact: true }),
  ).toBeVisible();
  await expect(history.getByText("/fixture/history", { exact: true })).not.toBeVisible();
  await history.getByText("存储与校验", { exact: true }).click();
  await expect(history.getByText("/fixture/history", { exact: true })).toBeVisible();
});

test("daily adapter submits catalog scope without minute fields and clears credentials on source changes", async ({
  page,
}) => {
  await mockCatalog(page);
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  const section = page.getByRole("region", { name: "历史数据", exact: true });
  await section
    .getByRole("combobox", { name: "数据源", exact: true })
    .selectOption("tushare.fut_daily");
  await section.getByLabel("品种代码", { exact: true }).fill("CU");
  await section.getByLabel("Tushare Token", { exact: true }).fill("daily-test-secret");
  await section.getByRole("button", { name: "查询月份合约", exact: true }).click();
  await section.getByLabel("月份合约", { exact: true }).selectOption("SHFE/cu/2023-10");
  let submitted = false;
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method !== "research.daily.submit") return route.fallback();
    expect(request.params.contract_id).toBe("SHFE/cu/2023-10");
    expect(request.params.catalog_cutoff_ns).toBe(catalogFixture.cutoff_ns);
    expect(request.params.token).toBe("daily-test-secret");
    expect(request.params).not.toHaveProperty("interval_minutes");
    expect(request.params).not.toHaveProperty("start");
    expect(request.params).not.toHaveProperty("end");
    submitted = true;
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    await route.fulfill({ response });
  });
  await section.getByRole("button", { name: "下载整个合约", exact: true }).click();
  await expect.poll(() => submitted).toBe(true);
  await expect(section.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
  await section.getByLabel("Tushare Token", { exact: true }).fill("daily-test-secret");
  await section
    .getByRole("combobox", { name: "数据源", exact: true })
    .selectOption("tushare.ft_mins");
  await expect(section.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
  await section
    .getByRole("combobox", { name: "数据源", exact: true })
    .selectOption("tushare.fut_daily");
  await expect(section.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "daily-test-secret",
  );
});

test("all contract months download as one task each within the request limit", async ({ page }) => {
  const months = {
    ...catalogFixture,
    items: [
      catalogFixture.items[0],
      { ...catalogFixture.items[0], code: "SHFE/cu/2023-11", name: "Copper 2311" },
    ],
  };
  let loaded = false;
  const submissions: Record<string, unknown>[] = [];
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (request.method === "research.contracts.load") loaded = true;
    if (request.method === "research.minutes.submit") submissions.push(request.params);
    if (
      !["research.contracts.load", "research.minutes.submit", "runtime.snapshot"].includes(
        request.method,
      )
    )
      return route.continue();
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    if (loaded) data.result.history_contracts = months;
    await route.fulfill({ response, json: data });
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  const section = page.getByRole("region", { name: "历史数据", exact: true });
  await section.getByLabel("品种代码", { exact: true }).fill("CU");
  await section.getByLabel("Tushare Token", { exact: true }).fill("ui-fixture-secret");
  await section.getByRole("button", { name: "查询月份合约", exact: true }).click();
  await section.getByLabel("月份合约", { exact: true }).selectOption("*");
  await section.getByLabel("每分钟请求上限", { exact: true }).fill("60");
  await section.getByLabel("Tushare Token", { exact: true }).fill("ui-fixture-secret");
  await section.getByRole("button", { name: "下载全部 2 个合约", exact: true }).click();
  await expect(section.getByText("已提交 2 / 2 个下载任务", { exact: true })).toBeVisible();
  expect(submissions.map(item => item.contract_id).sort()).toEqual([
    "SHFE/cu/2023-10",
    "SHFE/cu/2023-11",
  ]);
  expect(submissions.every(item => item.requests_per_minute === 30)).toBe(true);
  expect(new Set(submissions.map(item => item.id)).size).toBe(2);
  await expect(section.getByLabel("Tushare Token", { exact: true })).toHaveValue("");
});
