import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc, openResearch } from "./dataset-fixture";
import { openView } from "./navigation-helper";
test("fixed expression holdout displays original roles, crossing labels and tail without recomputation", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  const definition = (
    await rpc(page.request, "research.variable.register", {
      id: "held-definition-command",
      registration: {
        id: "held-definition",
        name: "原固定留出变量",
        purpose: "仅合成隔离资料，开发和留出统计分开",
        expression: "close / lag(close,window) - 1",
        window: 2,
      },
    })
  ).research_variable.definition;
  await seedDataset(
    page.request,
    Array.from({ length: 90 }, (_, i) => 100 + i + (i % 3)),
    "expression-holdout-ui",
  );
  await openView(page, "variables");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("button", { name: `原固定留出变量 · ${definition.object.id}`, exact: true })
    .click();
  await workspace.getByRole("button", { name: "用于因子研究", exact: true }).click();
  await expect(workspace.getByRole("combobox", { name: "固定变量选择", exact: true })).toHaveValue(
    definition.object.id,
  );
  await expect(workspace.getByLabel("回看 K 线数", { exact: true })).toHaveValue("2");
  await expect(workspace.getByRole("combobox", { name: "因子", exact: true })).toHaveCount(0);
  await expect(
    workspace
      .getByRole("region", { name: "原变量定义", exact: true })
      .getByText(definition.expression, { exact: true }),
  ).toBeVisible();
  await workspace.getByLabel("未来收益 K 线数", { exact: true }).fill("3");
  await workspace.getByRole("combobox", { name: "评价方式", exact: true }).selectOption("holdout");
  await workspace.getByLabel("前段 K 线数", { exact: true }).fill("45");
  await expect(workspace.getByRole("button", { name: "开始分析", exact: true })).toBeDisabled();
  await workspace.getByLabel("保存逐行特征与标签证据", { exact: true }).check();
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const task = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (value: { variable_definition?: { id: string } }) =>
            value.variable_definition?.id === definition.object.id,
        );
        if (task) id = task.id;
        return task?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const evidence = (await taskRpc(page.request, "research", "task.result.read", { id }))
    .task_result;
  expect(evidence.experiment.variable_definition).toEqual(definition);
  expect(evidence.experiment.evaluation).toEqual({ mode: "holdout", split_index: 45 });
  expect(
    evidence.result.partitions.map((part: { sample_count: number }) => part.sample_count),
  ).toEqual([40, 42]);
  expect(evidence.result.purged_count).toBe(3);
  expect(evidence.result.observations[42]).toMatchObject({
    role: "development",
    label_crosses_boundary: true,
    included: false,
  });
  expect(evidence.result.observations[45]).toMatchObject({ role: "holdout", included: true });
  await workspace.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "因子结果", exact: true });
  await expect(
    result.getByRole("region", { name: "前段", exact: true }).getByText("40", { exact: true }),
  ).toBeVisible();
  await expect(
    result.getByRole("region", { name: "留出段", exact: true }).getByText("42", { exact: true }),
  ).toBeVisible();
  await result.getByText("逐行特征与标签证据", { exact: true }).click();
  const rows = result
    .getByRole("table", { name: "逐行特征与标签证据", exact: true })
    .getByRole("row");
  await expect(rows.nth(3).getByRole("cell").nth(2)).toHaveText(
    String(evidence.result.observations[2].value),
  );
  await expect(rows.nth(43).getByText("标签跨开发边界，排除评价", { exact: true })).toBeVisible();
  await expect(rows.nth(43).getByText("开发段", { exact: true })).toBeVisible();
  await expect(rows.nth(46).getByText("参与评价", { exact: true })).toBeVisible();
  await expect(rows.nth(46).getByText("留出段", { exact: true })).toBeVisible();
  await result.getByRole("button", { name: "下一页逐行证据", exact: true }).click();
  await expect(rows.nth(40).getByRole("cell").nth(3)).toHaveText("未来标签尚未知");
  await expect(rows.nth(40).getByRole("cell").nth(4)).toHaveText("未来标签尚未知");
  const snapshot = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    service: snapshot.data.service,
    action: "stop",
  });
  await rpc(page.request, "node.action", {
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
    evidence,
  );
});
