import { test, expect } from "./test";
import type { Snapshot, HistoryPage } from "../src/bridge/client";

test("dataset viewer charts exact bars, pages, filters and retains the last page on failure", async ({
  page,
}) => {
  const start = 1692925200000000000n;
  let viewed: HistoryPage | null = null;
  let failNext = false;
  let failInitial = false;
  let online = true;
  let delayed: Promise<void> | null = null;
  let mismatchNext = false;
  await page.route("**/__asterion/api", async route => {
    const request = route.request().postDataJSON();
    if (!["runtime.snapshot", "data.minutes.page"].includes(request.method))
      return route.continue();
    if (request.method === "data.minutes.page" && (failNext || failInitial)) {
      failNext = false;
      return route.fulfill({ status: 503, body: "fixture read failure" });
    }
    if (request.method === "data.minutes.page") {
      const wait = delayed;
      delayed = null;
      if (wait) await wait;
      expect(request.params.id).toBe("viewer-fixture");
      expect(request.params).not.toHaveProperty("directory");
      const filtered = Boolean(request.params.start);
      const count = filtered ? 0 : 120;
      viewed = {
        id: "viewer-fixture",
        source: "tushare.ft_mins",
        contract_id: "SHFE/cu/2023-10",
        interval_minutes: 1,
        manifest_sha256: "b".repeat(64),
        total_rows: 120,
        matched_rows: count,
        offset: request.params.offset,
        limit: request.params.limit,
        first_ns: String(start),
        last_ns: String(start + 119n * 60000000000n),
        begin_ns: String(start),
        end_ns: String(start + 119n * 60000000000n),
        bars: Array.from(
          { length: Math.max(0, Math.min(request.params.limit, count - request.params.offset)) },
          (_, index) => ({
            timestamp_ns: String(start + BigInt(request.params.offset + index) * 60000000000n),
            open: "100.00000001",
            high: "102",
            low: "99",
            close: "101.5",
            volume: "20",
            amount: "12345678.12345678",
            open_interest: "1000",
          }),
        ),
      };
    }
    const response = await route.fetch({
      postData: { version: 1, method: "runtime.snapshot", params: {} },
    });
    const data = (await response.json()) as { result: Snapshot };
    if (!data.result.task_service || !data.result.data)
      return route.fulfill({ response, json: data });
    data.result.data.online = online;
    data.result.history_page = request.method === "data.minutes.page" ? viewed : null;
    if (request.method === "data.minutes.page" && mismatchNext && data.result.history_page) {
      data.result.history_page = { ...data.result.history_page, id: "different-dataset" };
      mismatchNext = false;
    }
    data.result.task_service!.tasks = [
      {
        kind: "minute_download",
        data_source: "tushare.ft_mins",
        id: "viewer-fixture",
        history_dataset_id: "viewer-fixture",
        state: "succeeded",
        attempt: 1,
        completed: 1,
        total: 1,
        error: "",
        result_digest: "b".repeat(64),
        trading_day: "",
        instrument: "SHFE/cu/2023-10",
        source_name: "Tushare SHFE/cu/2023-10",
        submission_sequence: 1,
        submitted_at_ms: Date.now(),
        updated_at_ms: Date.now(),
      },
    ];
    await route.fulfill({ response, json: data });
  });
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "查看数据", exact: true }).click();
  const viewer = page.getByRole("region", { name: "历史数据查看", exact: true });
  const assertTableAlignment = async () => {
    const offsets = await viewer.locator(".history-bars-table table").evaluate(table => {
      const edge = (element: Element, left: boolean) => {
        const range = document.createRange();
        range.selectNodeContents(element);
        const bounds = range.getBoundingClientRect();
        return left ? bounds.left : bounds.right;
      };
      const headings = [...table.querySelectorAll("th")];
      const cells = [...table.querySelectorAll("tbody tr:first-child td")];
      return headings.map((heading, index) =>
        Math.abs(edge(heading, index === 0) - edge(cells[index], index === 0)),
      );
    });
    for (const offset of offsets) expect(offset).toBeLessThan(1);
  };
  await expect(viewer.getByRole("img", { name: "合约 K 线与成交量" })).toBeVisible();
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  await expect(viewer.locator("tbody tr").first()).toContainText("100.00000001");
  await expect(viewer.locator("tbody tr").first()).toContainText("12345678.12345678");
  await assertTableAlignment();
  online = false;
  await expect(viewer.getByRole("status")).toContainText("历史数据服务未连接，查询暂不可用。");
  await expect(viewer.getByRole("status")).toContainText("已加载的图表与记录仍可查看。");
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  await expect(viewer.getByRole("img")).toBeVisible();
  await expect(viewer.getByRole("button", { name: "下一页", exact: true })).toBeDisabled();
  await expect(viewer.getByRole("button", { name: "查看区间", exact: true })).toBeDisabled();
  await expect(viewer.getByLabel("图表窗口", { exact: true })).toBeDisabled();
  await page.screenshot({ path: "build/history-viewer-disconnected.png" });
  online = true;
  await expect(viewer.getByRole("status")).toHaveCount(0);
  await expect(viewer.getByRole("button", { name: "下一页", exact: true })).toBeEnabled();
  await viewer.getByRole("img").focus();
  await page.keyboard.press("ArrowLeft");
  const chart = viewer.getByRole("img");
  await expect(viewer.locator(".price-chart-cursor")).toContainText("100.00000001");
  await chart.press("+");
  await expect(chart).toHaveAttribute("data-visible-count", "80");
  await expect(chart).toHaveAttribute("data-window-start", "20");
  await chart.press("Shift+ArrowLeft");
  await expect(chart).toHaveAttribute("data-window-start", "4");
  await chart.press("Home");
  await expect(chart).toHaveAttribute("data-window-start", "0");
  await chart.press("End");
  await expect(chart).toHaveAttribute("data-window-start", "20");
  await chart.press("-");
  await expect(chart).toHaveAttribute("data-visible-count", "100");

  await viewer.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(20);
  await expect(viewer.getByRole("button", { name: "下一页", exact: true })).toBeDisabled();
  failNext = true;
  await viewer.getByRole("button", { name: "上一页", exact: true }).click();
  await expect(viewer.getByRole("alert")).toBeVisible();
  await expect(viewer.locator("tbody tr")).toHaveCount(20);
  await viewer.getByLabel("图表窗口", { exact: true }).selectOption("25");
  await expect(viewer.locator("tbody tr")).toHaveCount(20);
  await expect(viewer.locator(".history-pagination")).toContainText("101–120 / 120");
  await viewer.getByRole("button", { name: "最早数据", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(25);
  await expect(viewer.getByRole("button", { name: "最早数据", exact: true })).toBeDisabled();
  await viewer.getByRole("button", { name: "最新数据", exact: true }).click();
  await expect(viewer.locator(".history-pagination")).toContainText("101–120 / 120");
  await expect(viewer.getByRole("button", { name: "最新数据", exact: true })).toBeDisabled();
  await viewer.getByLabel("开始时间（北京时间）").fill("2023-08-26T09:00");
  await viewer.getByRole("button", { name: "查看区间", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(0);
  await expect(viewer.getByText("此范围没有记录；不能据此判定缺失或无成交。")).toBeVisible();
  await expect(viewer.getByRole("button", { name: "最新数据", exact: true })).toBeDisabled();
  failNext = true;
  await viewer.getByRole("button", { name: "全部时间", exact: true }).click();
  await expect(viewer.getByRole("alert")).toBeVisible();
  await expect(viewer.getByLabel("开始时间（北京时间）")).toHaveValue("2023-08-26T09:00");
  await expect(viewer.locator("tbody tr")).toHaveCount(0);
  await viewer.getByRole("button", { name: "全部时间", exact: true }).click();
  await expect(viewer.getByLabel("开始时间（北京时间）")).toHaveValue("");
  await expect(viewer.locator("tbody tr")).toHaveCount(25);
  await viewer.getByText("质量与来源", { exact: true }).click();
  await expect(
    viewer.getByText("未校验交易日历完整性；缺失、休市与无成交尚不能自动区分。"),
  ).toBeVisible();
  await page.locator(".terminal-business").evaluate(element => element.scrollTo(0, 0));
  await page.screenshot({ path: "build/history-viewer-desktop.png" });
  await page.setViewportSize({ width: 800, height: 900 });
  await assertTableAlignment();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.locator(".terminal-business").evaluate(element => element.scrollTo(0, 0));
  await page.screenshot({ path: "build/history-viewer-compact.png" });
  await viewer.locator(".history-bars-table").scrollIntoViewIfNeeded();
  await page.screenshot({ path: "build/history-numeric-columns.png" });
  await viewer.getByRole("button", { name: "返回数据集", exact: true }).click();
  await expect(viewer).toHaveCount(0);
  await expect(page.getByRole("button", { name: "查看数据", exact: true })).toBeVisible();
  failInitial = true;
  await page.getByRole("button", { name: "查看数据", exact: true }).click();
  await expect(viewer.getByRole("alert")).toBeVisible();
  await expect(viewer.getByRole("img")).toHaveCount(0);
  failInitial = false;
  await viewer.getByRole("button", { name: "查看数据", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  mismatchNext = true;
  await viewer.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(viewer.getByRole("alert")).toContainText("返回的数据与所选数据集不匹配");
  await expect(viewer.locator("tbody tr")).toHaveCount(100);

  let release!: () => void;
  delayed = new Promise<void>(resolve => {
    release = resolve;
  });
  const lateResponse = page.waitForResponse(
    response =>
      response.url().endsWith("/__asterion/api") &&
      response.request().postDataJSON()?.method === "data.minutes.page" &&
      response.request().postDataJSON()?.params.offset === 100,
  );
  await viewer.getByRole("button", { name: "下一页", exact: true }).click();
  await expect(viewer).toHaveAttribute("aria-busy", "true");
  await viewer.getByRole("button", { name: "返回数据集", exact: true }).click();
  await page.getByRole("button", { name: "查看数据", exact: true }).click();
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  release();
  await lateResponse;
  await expect(viewer.locator("tbody tr")).toHaveCount(100);
  await expect(viewer.locator(".history-pagination")).toContainText("1–100 / 120");
  await viewer.getByRole("button", { name: "返回数据集", exact: true }).click();
  await page.addInitScript(() => localStorage.setItem("asterion.locale", "en-US"));
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "Data", exact: true }).click();
  await page.getByRole("button", { name: "View data", exact: true }).click();
  await expect(
    page.getByRole("region", { name: "Historical dataset viewer", exact: true }),
  ).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: "build/history-viewer-english.png" });
  // Polls keep arriving while the test finishes; ignore handlers whose
  // responses were disposed with the page.
  await page.unrouteAll({ behavior: "ignoreErrors" });
});
