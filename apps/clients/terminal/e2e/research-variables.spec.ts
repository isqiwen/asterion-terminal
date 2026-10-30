import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc, openResearch } from "./dataset-fixture";
import { openView } from "./navigation-helper";
test("fixed causal variables drive original factor rows and survive definition revision and restart", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  await openView(page, "variables");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(workspace.getByText("研究服务未连接", { exact: true })).toHaveCount(0);
  await workspace.getByLabel("变量名称", { exact: true }).fill("固定因果变量");
  await workspace
    .getByLabel("研究用途", { exact: true })
    .fill("合成隔离数据验证，不代表真实市场能力");
  await workspace.getByLabel("表达式", { exact: true }).fill("future(close)");
  await workspace.getByRole("button", { name: "检查表达式", exact: true }).click();
  await expect(workspace.getByText(/表达式检查未通过/)).toBeVisible();
  await expect(
    workspace.getByRole("button", { name: "登记不可变定义", exact: true }),
  ).toBeDisabled();
  await workspace.getByLabel("表达式", { exact: true }).fill("close / lag(close, window) - 1");
  await workspace.getByLabel("固定窗口", { exact: true }).fill("1");
  await workspace.getByRole("button", { name: "检查表达式", exact: true }).click();
  await expect(
    workspace.getByText("输出单位: 无量纲 · 实际预热观察数: 1", { exact: true }),
  ).toBeVisible();
  await workspace.getByRole("button", { name: "登记不可变定义", exact: true }).click();
  const original = workspace.getByRole("region", { name: "原变量定义", exact: true });
  await expect(original.getByText("close / lag(close, window) - 1", { exact: true })).toBeVisible();
  const definition = (
    await rpc(page.request, "research.variable.list", { after: "", limit: 30 })
  ).research_variable.definitions.find((value: { name: string }) => value.name === "固定因果变量");
  expect(definition.window).toBe(1);
  await seedDataset(
    page.request,
    Array.from({ length: 40 }, (_, i) => 100 + i + (i % 3)),
    "research-variable-fixed",
  );
  await original.getByRole("button", { name: "用于因子研究", exact: true }).click();
  await expect(workspace.getByRole("combobox", { name: "固定变量选择", exact: true })).toHaveValue(
    definition.object.id,
  );
  await expect(workspace.getByLabel("回看 K 线数", { exact: true })).toHaveValue("1");
  await expect(workspace.getByLabel("回看 K 线数", { exact: true })).toHaveAttribute(
    "readonly",
    "",
  );
  await workspace.getByLabel("保存逐行特征与标签证据", { exact: true }).check();
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const tasks = (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
        const task = tasks.find(
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
  expect(evidence.result.observations).toHaveLength(40);
  expect(evidence.result.observations[0].feature_state).toBe("FEATURE_WARMUP");
  expect(evidence.result.observations[1].value).toBeCloseTo(0.02, 12);
  await workspace.getByRole("button", { name: "查看结果", exact: true }).first().click();
  await workspace.getByText("逐行特征与标签证据", { exact: true }).click();
  await expect(
    workspace.getByRole("columnheader", { name: "特征值（无量纲）", exact: true }),
  ).toBeVisible();
  await openView(page, "variables");
  await workspace
    .getByRole("button", { name: `固定因果变量 · ${definition.object.id}`, exact: true })
    .click();
  await workspace.getByRole("button", { name: "基于此定义创建新草稿", exact: true }).click();
  await workspace.getByLabel("固定窗口", { exact: true }).fill("2");
  await workspace.getByRole("button", { name: "检查表达式", exact: true }).click();
  await expect(
    workspace.getByRole("button", { name: "登记不可变定义", exact: true }),
  ).toBeEnabled();
  await workspace.getByRole("button", { name: "登记不可变定义", exact: true }).click();
  await expect(workspace.getByLabel("固定窗口", { exact: true })).toBeDisabled();
  expect(
    (await rpc(page.request, "research.variable.read", { id: definition.object.id }))
      .research_variable.definition,
  ).toEqual(definition);
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
  expect(
    (await rpc(page.request, "research.variable.read", { id: definition.object.id }))
      .research_variable.definition,
  ).toEqual(definition);
});
