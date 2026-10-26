import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc, openResearch } from "./dataset-fixture";
import { openView } from "./navigation-helper";
test("fixed statistics read original columns independently and keep formal rows after restart without Data", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  await seedDataset(
    page.request,
    Array.from({ length: 90 }, (_, i) => 100 + i + (i % 3)),
    "statistics-ui-source",
  );
  await rpc(page.request, "factor.submit", {
    id: "statistics-source",
    series: { kind: "bars" },
    factor: "momentum",
    lookbacks: [2],
    horizon: 3,
    evaluation: { mode: "holdout", split_index: 45 },
    retain_observations: true,
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (task: { id: string }) => task.id === "statistics-source",
        )?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const source = (
    await taskRpc(page.request, "research", "task.result.read", { id: "statistics-source" })
  ).task_result;
  expect(source.result.partitions[0].sample_count).toBe(40);
  await openView(page, "statistics");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("combobox", { name: "来源研究结果", exact: true })
    .selectOption("statistics-source");
  await workspace.getByRole("button", { name: "固定原结果", exact: true }).click();
  await expect(workspace.getByLabel("结束观察索引（不含）", { exact: true })).toHaveValue("90");
  await workspace.getByLabel("统计用途", { exact: true }).fill("合成原开发特征的独立分布");
  await workspace.getByRole("button", { name: "提交或确认原统计任务", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const value = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (task: { kind: string }) => task.kind === "statistics",
        );
        if (value) id = value.id;
        return value?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const first = (await taskRpc(page.request, "research", "task.result.read", { id })).task_result;
  expect(first.result.summary).toMatchObject({ total: 90, allowed: 45, valid: 43, missing: 2 });
  expect(first.result.rows[42]).toMatchObject({
    reason: "VALID",
    value: source.result.observations[42].value,
  });
  expect(first.result.rows[45]).toMatchObject({ reason: "OTHER_ROLE", value: null, bin: null });
  expect(first.experiment.source.program_artifact).toBe(source.task.program_artifact);
  await workspace.getByRole("button", { name: "查看结果", exact: true }).click();
  const report = workspace.getByRole("region", { name: "正式统计报告", exact: true });
  await expect(
    report.getByText("实际有效数", { exact: true }).locator("..").getByText("43", { exact: true }),
  ).toBeVisible();
  const rows = report.getByRole("table", { name: "统计逐行证据", exact: true }).getByRole("row");
  await expect(rows.nth(43).getByRole("cell").nth(2)).toHaveText(
    String(first.result.rows[42].value),
  );
  await expect(rows.nth(46).getByRole("cell").nth(2)).toHaveText("—");
  const histogram = report.getByRole("region", { name: "原报告直方图", exact: true });
  await histogram.getByRole("button").nth(1).click();
  await expect(rows).toHaveCount(Math.min(first.result.bins[0].count, 50) + 1);
  await expect(rows.nth(1).getByRole("cell").nth(4)).toHaveText("0");
  await workspace.getByRole("button", { name: "新建统计请求", exact: true }).click();
  await workspace
    .getByRole("combobox", { name: "统计列", exact: true })
    .selectOption("forward_return");
  await workspace
    .getByLabel("统计用途", { exact: true })
    .fill("合成未来标签独立分布；不反向筛选特征");
  const snapshot = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    id: "local",
    service: snapshot.data.service,
    action: "stop",
  });
  await workspace.getByRole("button", { name: "提交或确认原统计任务", exact: true }).click();
  let labelsId = "";
  await expect
    .poll(
      async () => {
        const value = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (task: { kind: string; id: string }) => task.kind === "statistics" && task.id !== id,
        );
        if (value) labelsId = value.id;
        return value?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const labels = (await taskRpc(page.request, "research", "task.result.read", { id: labelsId }))
    .task_result;
  expect(labels.result.summary.valid).toBe(42);
  expect(labels.result.rows[0].reason).toBe("VALID");
  expect(labels.result.rows[42]).toMatchObject({ reason: "CROSSED_LABEL", value: null });
  await rpc(page.request, "node.action", {
    id: "local",
    service: snapshot.research_service.service,
    action: "restart",
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.find(
            (item: { id: string }) => item.id === snapshot.research_service.service,
          ).health,
      { timeout: 20000 },
    )
    .toBe("ready");
  await openResearch(page.request);
  expect((await taskRpc(page.request, "research", "task.result.read", { id })).task_result).toEqual(
    first,
  );
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: labelsId })).task_result,
  ).toEqual(labels);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "statistics-source" }))
      .task_result,
  ).toEqual(source);
});
