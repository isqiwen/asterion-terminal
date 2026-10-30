import { taskRpc, openData } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { resolve } from "node:path";
import { createHash } from "node:crypto";
import { writeFile } from "node:fs/promises";
import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";

test("F07 explicitly refreshes one stored day and preserves old experiments and unchanged revisions", async ({
  page,
}, info) => {
  test.setTimeout(120000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const build = process.env.ASTERION_CPP_BUILD ?? resolve(__dirname, "../../../../build/Debug");
  const path = resolve(
    build,
    "tests/support/native-plugin/good/plugin_fixture_good" +
      (process.platform === "darwin" ? ".dylib" : ".so"),
  );
  const candidate = (await rpc(page.request, "native.plugins.preview", { path })).plugin_candidate;
  await rpc(page.request, "native.plugins.install", { path, sha256: candidate.sha256 });
  const state = await rpc(page.request, "node.action", {
    service: "historical-data",
    action: "stop",
  });
  const service = state.nodes
    .find((node: { id: string }) => node.id === "local")
    .health.services.find((item: { id: string }) => item.id === "historical-data");
  await rpc(page.request, "node.plugins.configure", {
    service: "historical-data",
    revision: service.revision,
    plugins: [
      ...state.native_plugins.items
        .filter(
          (item: { managed?: boolean; selectable: boolean }) => !item.managed && item.selectable,
        )
        .map((item: { sha256: string }) => item.sha256),
      candidate.sha256,
    ],
  });
  await openData(page.request);
  const days = Array.from({ length: 56 }, (_, i) =>
    new Date(Date.UTC(2024, 2, 1 + i)).toISOString().slice(0, 10),
  ).filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()));
  const seeded = await seedHistory(page.request, [100], "refresh-baseline", {
    tablesOnly: true,
    provider: "test.independent.c",
    product: "cu",
    month: "2024-03",
    dailyDays: days,
  });
  expect(seeded.daily.daily_source).toBe("fixture.daily.v1");
  // Synthetic test credential is kept only in the disposable test profile.
  await rpc(page.request, "data.credentials.save", {
    provider: "test.independent.c",
    credential: "f07-fixture-secret",
    remember: false,
    requests_per_minute: 60,
  });
  await rpc(page.request, "data.download.budget.configure", {
    source: "fixture.minutes",
    token: "",
    requests_per_minute: 60,
  });
  await rpc(page.request, "factor.submit", {
    id: "f07-old-factor",
    series: {
      kind: "tables",
      snapshot: seeded.daily.snapshot,
      daily_source: seeded.daily.daily_source,
      calendar_source: seeded.daily.calendar_source,
      end_day: days.at(-1)!,
      ranges: [{ begin_day: days[0], instruments: ["SHFE/cu/2024-03"] }],
    },
    factor: "momentum",
    lookbacks: [2],
    horizon: 1,
    evaluation: { mode: "full_sample" },
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (task: { id: string }) => task.id === "f07-old-factor",
        )?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const old = (
    await taskRpc(page.request, "research", "task.result.read", { id: "f07-old-factor" })
  ).task_result;
  const oldHead = (await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot;
  const pageQuery = {
    table: "futures.daily",
    exchange: "SHFE",
    period: days[0],
    source: "fixture.daily.v1",
    offset: 0,
    limit: 10,
  };
  const oldPage = (await rpc(page.request, "data.tables.page", pageQuery)).table_page;
  expect(oldPage.response_evidence).toBeNull();

  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "tables");
  const tables = page.getByRole("region", { name: "全市场数据", exact: true });
  await tables
    .getByRole("table", { name: "全市场数据覆盖" })
    .getByRole("row")
    .filter({ hasText: "日线" })
    .getByRole("button", { name: "查看数据", exact: true })
    .click();
  const partition = tables.getByRole("region", { name: "分区数据", exact: true });
  await partition.getByLabel("交易日", { exact: true }).fill(days[0]);
  await partition.getByRole("button", { name: "查看", exact: true }).click();
  let refresh = partition.getByRole("region", { name: "重新获取此日分区", exact: true });
  await refresh.getByRole("button", { name: "预览重新获取", exact: true }).click();
  await expect(refresh).toContainText("预览未下载数据");
  expect((await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot).toBe(oldHead);
  await refresh.getByRole("button", { name: "确认重新获取此分区", exact: true }).click();
  await expect(
    refresh.getByRole("status").filter({ hasText: "重新获取完成：产生新修订" }),
  ).toBeVisible({ timeout: 30000 });
  const newHead = (await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot;
  expect(newHead).not.toBe(oldHead);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "f07-old-factor" }))
      .task_result,
  ).toEqual(old);
  await refresh.getByRole("button", { name: "读取当前分区", exact: true }).click();
  refresh = partition.getByRole("region", { name: "重新获取此日分区", exact: true });
  await expect(partition.getByRole("table", { name: "分区数据", exact: true })).toContainText(
    "72000.5",
  );
  const capturedPage = (await rpc(page.request, "data.tables.page", pageQuery)).table_page;
  expect(oldPage.response_review.status).toBe("unknown");
  expect(oldPage.response_review.reason).toBe("raw_missing");
  // The synthetic C plugin omits open_interest_change from its normalized
  // row while its hand-written body contains oi_chg=0. Keep this fixture
  // inconsistency as a quality-review regression; production Tushare preserves
  // zero and null separately (ResponseInventory core test).
  const quality = capturedPage.response_review;
  expect(quality.status).toBe("differences");
  expect(quality).toMatchObject({
    response_fields: 14,
    response_rows: 1,
    normalized_rows: 1,
    dated_rows: 1,
    matched_rows: 1,
    compared_cells: 12,
    differences: [
      {
        kind: "value_mismatch",
        key: "CU2403.SHF",
        field: "open_interest_change",
        raw: "0",
        expected: "0",
        actual: null,
      },
    ],
    skipped_keys: [],
    unknown_keys: [],
  });
  const firstObservation = capturedPage.latest_refresh_observation;
  expect(firstObservation.kind).toBe("explicit_refresh_response_observation");
  expect(firstObservation.id).toMatch(/^[0-9a-f]{64}$/);
  expect(firstObservation.selected_snapshot).toBe(oldPage.revision.snapshot);
  expect(firstObservation.revision.snapshot).toBe(newHead);
  expect(firstObservation.response_sha256).toBe(
    createHash("sha256").update(firstObservation.body).digest("hex"),
  );
  expect(firstObservation.content_changed_from_selected).toBe(true);
  const evidence = capturedPage.response_evidence;
  expect(evidence.kind).toBe("raw_response_body");
  expect(evidence.format).toBe("tushare.fut_daily.raw.v1");
  expect(evidence.sha256).toBe(createHash("sha256").update(evidence.body).digest("hex"));
  expect(evidence.bytes).toBe(Buffer.byteLength(evidence.body));
  expect(BigInt(evidence.received_at_ns)).toBeGreaterThan(0n);
  expect(evidence.revision_snapshot).toBe(newHead);
  expect(evidence.normalized_content).toBe(capturedPage.revision.content);
  expect(evidence.normalized_object).toMatch(/^[0-9a-f]{64}$/);
  expect(evidence.provider_artifact).toBe(candidate.sha256);
  expect(evidence.body).toContain("43200000.000025");
  expect(capturedPage.rows[0][capturedPage.columns.indexOf("amount")]).toBe("432000000000.25");
  for (const sensitive of ["token", "cookie", "Authorization"])
    expect(evidence.body).not.toContain(sensitive);
  await partition.getByText("响应与分区质量核对", { exact: true }).click();
  const review = partition.getByRole("region", { name: "响应与分区质量核对", exact: true });
  await expect(review).toContainText("发现差异");
  await expect(review.getByRole("table", { name: "响应分区差异", exact: true })).toContainText(
    "open_interest_change",
  );
  await expect(review.getByRole("table", { name: "响应分区差异", exact: true })).toContainText(
    "缺失",
  );
  await expect(review).toContainText("比较 12 个数值单元格");
  await review.getByText("本期字段与单位规则", { exact: true }).click();
  await expect(
    review.getByRole("table", { name: "本期字段与单位规则", exact: true }),
  ).toContainText("原值乘10000");
  await review.scrollIntoViewIfNeeded();
  await page.screenshot({ path: info.outputPath("response-quality.png"), fullPage: true });
  await partition.getByText("原始响应体证据", { exact: true }).click();
  await expect(partition.getByLabel("原始响应体原文", { exact: true })).toHaveText(evidence.body);
  await partition.getByLabel("原始响应体原文", { exact: true }).scrollIntoViewIfNeeded();
  const shot = info.outputPath("raw-response-evidence.png");
  await page.screenshot({ path: shot, fullPage: true });
  await info.attach("raw response and normalized revision", {
    path: shot,
    contentType: "image/png",
  });
  const evidencePath = info.outputPath("raw-response-evidence.json");
  await writeFile(
    evidencePath,
    JSON.stringify({ old_revision: oldPage.revision, current: capturedPage }, null, 2),
  );
  await info.attach("verified raw response evidence", {
    path: evidencePath,
    contentType: "application/json",
  });
  await refresh.getByRole("button", { name: "预览重新获取", exact: true }).click();
  await refresh.getByRole("button", { name: "确认重新获取此分区", exact: true }).click();
  await expect(
    refresh.getByRole("status").filter({ hasText: "重新获取完成：内容未变，未新增修订" }),
  ).toBeVisible({ timeout: 30000 });
  expect((await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot).toBe(newHead);
  const completed = (await rpc(page.request, "runtime.snapshot")).data.tasks.filter(
    (task: { table_refresh?: unknown }) => task.table_refresh,
  );
  expect(completed).toHaveLength(2);
  expect(completed.every((task: { state: string }) => task.state === "succeeded")).toBe(true);
  expect(
    (await rpc(page.request, "data.tables.page", pageQuery)).table_page.response_evidence,
  ).toEqual(evidence);
  expect(
    (await rpc(page.request, "data.tables.page", pageQuery)).table_page.response_review,
  ).toEqual(quality);
  const secondPage = (await rpc(page.request, "data.tables.page", pageQuery)).table_page;
  const secondObservation = secondPage.latest_refresh_observation;
  expect(secondObservation.id).not.toBe(firstObservation.id);
  expect(secondObservation.unchanged).toBe(true);
  expect(secondObservation.content_changed_from_selected).toBe(false);
  expect(secondObservation.selected_snapshot).toBe(newHead);
  expect(secondObservation.revision.snapshot).toBe(newHead);
  expect(secondObservation.response_sha256).toBe(firstObservation.response_sha256);
  expect(secondPage.response_evidence).toEqual(evidence);
  const firstAcquisition = completed.find(
    (task: { table_refresh?: { previous_snapshot: string } }) =>
      task.table_refresh?.previous_snapshot === oldPage.revision.snapshot,
  );
  expect(firstAcquisition).toBeTruthy();
  const heldSnapshot = (
    await rpc(page.request, "data.tables.retirement.preview", { id: oldPage.revision.snapshot })
  ).table_retirement;
  expect(heldSnapshot.state).toBe("blocked");
  expect(heldSnapshot.acquisitions).toContain(firstAcquisition.id);
  expect(
    (
      await rpc(page.request, "data.tables.retirement.execute", {
        id: oldPage.revision.snapshot,
        preview_digest: heldSnapshot.preview_digest,
      })
    ).table_retirement.state,
  ).toBe("blocked");
  const researchHold = (await rpc(page.request, "data.tables.retirement.preview", { id: oldHead }))
    .table_retirement;
  expect(researchHold.state).toBe("blocked");
  expect(
    researchHold.consumers.some((consumer: { id: string }) => consumer.id === "f07-old-factor"),
  ).toBe(true);
  await refresh.getByRole("button", { name: "维护采集记录", exact: true }).click();
  const maintenance = refresh.getByRole("region", { name: "采集记录维护", exact: true });
  await expect(maintenance).toContainText("采集记录可退役");
  const retiredId = await maintenance.locator("code").innerText();
  expect(completed.some((task: { id: string }) => task.id === retiredId)).toBe(true);
  const originalAcquisition = (
    await taskRpc(page.request, "data", "task.result.read", { id: retiredId })
  ).task_result;
  await expect(
    maintenance.getByRole("button", { name: "确认采集退役", exact: true }),
  ).toBeDisabled();
  await maintenance.getByRole("checkbox", { name: "确认退役采集记录并禁止重试" }).check();
  await maintenance.getByRole("button", { name: "确认采集退役", exact: true }).click();
  await expect(maintenance).toContainText("采集记录已退役");
  const retiredAcquisition = (
    await taskRpc(page.request, "data", "task.result.read", { id: retiredId })
  ).task_result;
  expect(retiredAcquisition.task.retired).toBe(true);
  expect(retiredAcquisition.task.result_digest).toBe(originalAcquisition.task.result_digest);
  expect(retiredAcquisition.table_update).toEqual(originalAcquisition.table_update);
  await taskRpc(page.request, "data", "task.retirement.execute", { id: firstAcquisition.id });
  expect(
    (await rpc(page.request, "data.tables.retirement.preview", { id: oldPage.revision.snapshot }))
      .table_retirement.acquisitions,
  ).toHaveLength(0);
  expect((await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot).toBe(newHead);
  await refresh.getByRole("button", { name: "读取当前分区", exact: true }).click();
  await partition.getByText("独立重取观测", { exact: true }).click();
  const observed = partition.getByRole("region", { name: "独立重取观测", exact: true });
  await expect(observed).toContainText(secondObservation.id);
  await expect(observed).toContainText("不是旧修订首次导入原文");
  await expect(observed).toContainText("规范化内容与预览所选修订相同");
  await observed.scrollIntoViewIfNeeded();
  await page.screenshot({ path: info.outputPath("refresh-observation.png"), fullPage: true });
  await writeFile(
    info.outputPath("refresh-observations.json"),
    JSON.stringify(
      {
        old_revision: oldPage.revision,
        changed: firstObservation,
        unchanged: secondObservation,
        original_revision_evidence_preserved: secondPage.response_evidence,
      },
      null,
      2,
    ),
  );
  const beforeRestart = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    service: beforeRestart.data.service,
    action: "stop",
  });
  await openData(page.request);
  await expect
    .poll(async () => !!(await rpc(page.request, "runtime.snapshot")).data?.online, {
      timeout: 30000,
    })
    .toBe(true);
  expect(
    (await rpc(page.request, "data.tables.page", pageQuery)).table_page.response_evidence,
  ).toEqual(evidence);

  expect(
    (await taskRpc(page.request, "data", "task.retirement.preview", { id: retiredId }))
      .task_retirement.state,
  ).toBe("retired");
  expect(
    (await rpc(page.request, "data.tables.page", pageQuery)).table_page.response_review,
  ).toEqual(quality);
  expect(
    (await rpc(page.request, "data.tables.page", pageQuery)).table_page.latest_refresh_observation,
  ).toEqual(secondObservation);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "f07-old-factor" }))
      .task_result,
  ).toEqual(old);
});
