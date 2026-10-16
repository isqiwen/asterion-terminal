import { readFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { seedHistory, rpc } from "./dataset-fixture";
import { test, expect } from "./test";
test("daily factor reuse keeps original snapshots or explicitly chooses revised data", async ({
  page,
}) => {
  test.setTimeout(60000);
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  const days = Array.from({ length: 50 }, (_, i) =>
    new Date(Date.UTC(2026, 7, 20 + i)).toISOString().slice(0, 10),
  ).filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()));
  for (const [i, product] of ["rb", "cu", "al"].entries())
    await seedHistory(page.request, [100], `liquidity-${product}`, {
      product,
      tablesOnly: true,
      dailyDays: days,
      keep: i > 0,
      openInterest: product === "cu" ? 50 : 200,
    });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await page.getByRole("button", { name: "因子", exact: true }).click();
  await workspace.getByRole("button", { name: "全市场数据表", exact: true }).click();
  const input = workspace.getByLabel("品种或合约", { exact: true });
  for (const product of ["rb", "cu", "al"]) {
    await input.fill(`SHFE/${product}/2026-10`);
    await workspace.getByRole("button", { name: "添加", exact: true }).click();
  }
  await workspace.getByLabel("起始日期", { exact: true }).fill(days[0]);
  await workspace.getByLabel("结束日期", { exact: true }).fill(days.at(-1)!);
  await workspace.getByLabel("回看日线数", { exact: true }).fill("2");
  await workspace.getByLabel("未来日线数", { exact: true }).fill("1");
  await workspace.getByLabel("启用历史流动性筛选", { exact: true }).check();
  await workspace.getByLabel("此前连续观测数", { exact: true }).fill("3");
  await workspace.getByLabel("上一观测最低成交量（手）", { exact: true }).fill("0");
  await workspace.getByLabel("上一观测最低持仓量（手）", { exact: true }).fill("100");
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  const task = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "SHFE/rb/2026-10" });
  await expect(task.getByText("已完成", { exact: true })).toBeVisible({ timeout: 20000 });
  await task.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "因子结果", exact: true });
  await result.getByText("每日资格与排除原因", { exact: true }).click();
  await result.getByLabel("资格日期", { exact: true }).fill(days[5]);
  const members = result.getByRole("table", { name: "每日成员资格", exact: true });
  await expect(members.getByRole("row").filter({ hasText: "SHFE/cu/2026-10" })).toContainText(
    "上一观测持仓量不足",
  );
  await expect(members.getByRole("row").filter({ hasText: "SHFE/rb/2026-10" })).toContainText(
    "有资格",
  );
  await expect(result.getByText("实际评价成员数", { exact: false })).toContainText("2");
  const state = await rpc(page.request, "runtime.snapshot", {});
  expect(state.task_result.experiment.liquidity).toEqual({
    minimum_history: 3,
    minimum_volume: "0",
    minimum_open_interest: "100",
  });
  expect(state.task_result.result.memberships).toHaveLength(days.length * 3);
  const id = state.task_result.id;
  await result.getByText("截面明细", { exact: true }).click();
  const sections = result.getByRole("table", { name: "截面明细", exact: true });
  await sections
    .locator("tbody tr")
    .filter({ has: page.locator("td:first-child").filter({ hasText: days[5] }) })
    .getByRole("button", { name: "查看成员", exact: true })
    .click();
  const evidence = result.getByRole("region", { name: "截面成员证据", exact: true });
  await expect(evidence).toContainText(`${days[5]} → ${days[6]}`);
  await expect(evidence).toContainText("实际窗口: 2");
  const details = evidence.getByRole("table", { name: "截面成员明细", exact: true });
  await expect(details.locator("tbody tr")).toHaveCount(2);
  await expect(details).toContainText("SHFE/rb/2026-10");
  await expect(details).toContainText("SHFE/al/2026-10");
  await expect(details).not.toContainText("SHFE/cu/2026-10");
  await expect(details).toContainText("0.0000%");
  // The first evaluated observation has not met the prior-history requirement.
  await sections.getByRole("button", { name: "查看成员", exact: true }).first().click();
  await expect(evidence).toContainText("此观测没有实际参与成员");
  await expect(evidence.getByRole("table")).toHaveCount(0);
  const rejected = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "factor.reuse",
      params: {
        id: `factor-${crypto.randomUUID()}`,
        source_id: id,
        source_result_digest: "f".repeat(64),
        experiment: state.task_result.experiment,
        mode: "fixed",
        snapshot: "",
      },
    },
  });
  const refusal = await rejected.json();
  expect(refusal.error).toBeDefined();
  expect(JSON.stringify(refusal.error)).toContain(
    "experiment evidence changed; reopen its result before reuse",
  );
  // A newer immutable revision changes liquidity, while the original remains readable.
  await seedHistory(page.request, [100], "revised-cu", {
    product: "cu",
    tablesOnly: true,
    dailyDays: days,
    keep: true,
    openInterest: 250,
  });
  await page.reload();
  await rpc(page.request, "task.result", { id });
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await page.getByRole("button", { name: "因子", exact: true }).click();
  const originalRow = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ has: page.getByText(id, { exact: true }) });
  await originalRow.getByRole("button", { name: "查看结果", exact: true }).click();
  const createFrom = workspace.getByRole("region", { name: "按此日线实验创建", exact: true });
  await createFrom.getByLabel("沿用原固定快照", { exact: true }).check();
  await createFrom.getByRole("button", { name: "创建独立因子任务", exact: true }).click();
  const fixedId = (await createFrom.getByRole("status").locator("code").textContent())!;
  expect(fixedId).not.toBe(id);
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).task_service.tasks.find(
          (task: { id: string }) => task.id === fixedId,
        )?.state,
    )
    .toBe("succeeded");
  const fixed = (await rpc(page.request, "task.result", { id: fixedId })).task_result;
  expect(fixed.experiment).toEqual(state.task_result.experiment);
  expect(fixed.result).toEqual(state.task_result.result);
  await rpc(page.request, "task.result", { id });
  await originalRow.getByRole("button", { name: "查看结果", exact: true }).click();
  await createFrom.getByLabel("选择当前服务的新快照", { exact: true }).check();
  await expect(
    createFrom.getByRole("button", { name: "创建独立因子任务", exact: true }),
  ).toBeEnabled();
  await createFrom.getByRole("button", { name: "创建独立因子任务", exact: true }).click();
  const newId = (await createFrom.getByRole("status").locator("code").textContent())!;
  expect(newId).not.toBe(id);
  expect(newId).not.toBe(fixedId);
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).task_service.tasks.find(
          (task: { id: string }) => task.id === newId,
        )?.state,
    )
    .toBe("succeeded");
  const fresh = (await rpc(page.request, "task.result", { id: newId })).task_result;
  expect(fresh.experiment.liquidity).toEqual(state.task_result.experiment.liquidity);
  expect(fresh.experiment.series[0].data.snapshot).not.toBe(
    state.task_result.experiment.series[0].data.snapshot,
  );
  expect(fresh.experiment.dataset_revision).not.toBe(state.task_result.experiment.dataset_revision);
  const section = fresh.result.cross_sections.find(
    (row: { observed: string }) => row.observed === days[5],
  );
  expect(section.series).toBe(3);
  expect(section.members).toHaveLength(3);
  const unchanged = (await rpc(page.request, "task.result", { id })).task_result;
  expect(unchanged.experiment).toEqual(state.task_result.experiment);
  expect(unchanged.result).toEqual(state.task_result.result);
  const comparison = workspace.getByRole("region", { name: "日线因子比较", exact: true });
  await comparison.getByLabel(id + " · " + unchanged.task.instrument, { exact: true }).check();
  await comparison.getByLabel(fixedId + " · " + unchanged.task.instrument, { exact: true }).check();
  await comparison.getByLabel(newId + " · " + unchanged.task.instrument, { exact: true }).check();
  const before = (await rpc(page.request, "runtime.snapshot")).task_result;
  await comparison.getByRole("button", { name: "读取并比较", exact: true }).click();
  const compared = comparison.getByRole("table", { name: "因子实验对照", exact: true });
  await expect(compared).toBeVisible();
  const method = compared
    .getByRole("row")
    .filter({ has: page.getByRole("rowheader", { name: "比较口径", exact: true }) });
  await expect(method).toContainText("同口径对照");
  await expect(method).toContainText("仅独立列示");
  await expect(method).toContainText("固定快照不同");
  await expect(compared).toContainText(unchanged.experiment.series[0].data.snapshot);
  await expect(compared).toContainText(fresh.experiment.series[0].data.snapshot);
  const visibleMatrixRows = await compared.evaluate(element => {
    const table = element as HTMLTableElement;
    return Array.from(table.tBodies[0].rows).map(row => ({
      label: row.cells[0].textContent?.trim() ?? "",
      cells: Array.from(row.cells)
        .slice(1)
        .map(cell => ({
          text: cell.textContent?.trim() ?? "",
          state: cell.dataset.valueState ?? "display_only",
          metrics: Array.from(cell.querySelectorAll<HTMLElement>("dd[data-value-state]")).map(
            value => ({
              label: value.previousElementSibling?.textContent?.trim() ?? "",
              text: value.textContent?.trim() ?? "",
              state: value.dataset.valueState!,
            }),
          ),
        })),
    }));
  });
  const comparisonExport = comparison.getByRole("region", { name: "比较证据导出", exact: true });
  const comparisonDownloadEvent = page.waitForEvent("download");
  await comparisonExport.getByRole("button", { name: "导出当前比较证据", exact: true }).click();
  const comparisonDownload = await comparisonDownloadEvent;
  const comparisonBytes = await readFile((await comparisonDownload.path())!);
  const comparisonDocument = JSON.parse(comparisonBytes.toString("utf8"));
  expect(comparisonDocument).toMatchObject({
    format: "asterion.research.comparison.evidence",
  });
  expect(comparisonDocument.matrix.columns.map((column: { id: string }) => column.id)).toEqual([
    id,
    fixedId,
    newId,
  ]);
  expect(comparisonDocument.matrix.rows).toEqual(visibleMatrixRows);
  expect(comparisonDocument.matrix.columns[2].methodology).toBe("independent");
  expect(comparisonDocument.matrix.columns[2].reasons).toContain("固定快照不同");
  const methodRow = comparisonDocument.matrix.rows.find(
    (row: { label: string }) => row.label === "比较口径",
  );
  for (const reason of comparisonDocument.matrix.columns[2].reasons)
    expect(methodRow.cells[2].text).toContain(reason);
  const originals = [unchanged, fixed, fresh];
  comparisonDocument.experiments.forEach(
    (member: { experiment: unknown; result: unknown }, i: number) => {
      expect(member.experiment).toEqual(originals[i].experiment);
      expect(member.result).toEqual(originals[i].result);
    },
  );
  await expect(comparisonExport.getByRole("status")).toContainText(
    createHash("sha256").update(comparisonBytes).digest("hex"),
  );

  expect((await rpc(page.request, "runtime.snapshot")).task_result).toEqual(before);
  expect((await rpc(page.request, "task.result.read", { id })).task_result).toEqual(unchanged);
  expect((await rpc(page.request, "task.result.read", { id: newId })).task_result).toEqual(fresh);
  expect((await rpc(page.request, "runtime.snapshot")).task_result).toEqual(before);
  expect(fixed.task.metadata).toMatchObject({
    source_id: id,
    source_result_digest: unchanged.task.result_digest,
    data_choice: "fixed",
    snapshot: "",
  });
  expect(fresh.task.metadata).toMatchObject({
    source_id: id,
    data_choice: "snapshot",
    snapshot: fresh.experiment.series[0].data.snapshot,
  });
  await originalRow.getByText("名称、备注与来源", { exact: true }).click();
  const description = originalRow.getByRole("region", { name: "实验描述", exact: true });
  await description.getByLabel("实验名称", { exact: true }).fill("liquidity baseline");
  await description.getByLabel("实验备注", { exact: true }).fill("fixed evidence\nreviewed");
  await description.getByRole("button", { name: "保存描述", exact: true }).click();
  await expect(description.getByRole("status")).toHaveText("描述已保存");
  await comparisonExport.getByRole("button", { name: "导出当前比较证据", exact: true }).click();
  await expect(comparisonExport).toContainText(
    "实验结果或名称备注已变化，请重新读取比较后再导出。",
  );
  await expect(comparisonExport.getByRole("status")).toHaveCount(0);

  const edited = (await rpc(page.request, "task.result.read", { id })).task_result;
  expect(edited.task.metadata).toMatchObject({
    name: "liquidity baseline",
    note: "fixed evidence\nreviewed",
    revision: 1,
  });
  expect(edited.task.result_digest).toBe(unchanged.task.result_digest);
  expect(edited.experiment).toEqual(unchanged.experiment);
  expect(edited.result).toEqual(unchanged.result);
  const exportPanel = workspace.getByRole("region", { name: "研究证据导出", exact: true });
  const downloadEvent = page.waitForEvent("download");
  await exportPanel.getByRole("button", { name: "导出研究证据", exact: true }).click();
  const download = await downloadEvent;
  const bytes = await readFile((await download.path())!);
  const document = JSON.parse(bytes.toString("utf8"));
  expect(document).toMatchObject({
    format: "asterion.research.evidence",
    source: {
      task_id: id,
      kind: "factor",
      input_sha256: null,
      result_sha256: edited.task.result_digest,
    },
  });
  expect(document.description).toEqual(edited.task.metadata);
  expect(document.experiment).toEqual(edited.experiment);
  expect(document.result).toEqual(edited.result);
  expect(document.not_included).toContain("input.pb");
  expect(download.suggestedFilename()).toBe(id + ".evidence.json");
  await expect(exportPanel.getByRole("status")).toContainText(
    createHash("sha256").update(bytes).digest("hex"),
  );
  expect((await rpc(page.request, "task.result.read", { id })).task_result).toEqual(edited);
  await page.reload();
  const restored = (await rpc(page.request, "task.metadata.read", { id })).task_description;
  expect(restored.metadata).toEqual(edited.task.metadata);
});
