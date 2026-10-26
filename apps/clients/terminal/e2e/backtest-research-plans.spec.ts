import { test, expect } from "./test";
import { rpc, seedDataset, taskRpc, openResearch } from "./dataset-fixture";
import { openView } from "./navigation-helper";

test("backtest retains its admitted research plan and independent human judgments across criteria revision and restart", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  const initial = await rpc(page.request, "runtime.snapshot");
  const dataService = initial.data?.service ?? "historical-data";
  await rpc(page.request, "node.action", { id: "local", service: dataService, action: "stop" });
  await openView(page, "projects");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(workspace.getByText("研究服务未连接", { exact: true })).toHaveCount(0);
  await workspace.getByLabel("项目标题", { exact: true }).fill("独立回测研究计划证据");
  await workspace
    .getByLabel("研究问题", { exact: true })
    .fill("合成序列上的动量与未来收益是否有关？");
  await workspace
    .getByLabel("可检验假设", { exact: true })
    .fill("动量正相关；仅合成隔离数据，不代表真实市场能力。");
  await workspace
    .getByLabel("支持标准", { exact: true })
    .fill("原始支持标准：核对留出结果，不由单个IC自动判断。");
  await workspace
    .getByLabel("否定标准", { exact: true })
    .fill("原始否定标准：反向关系或证据不足。");
  await workspace.getByRole("button", { name: "保存研究草稿", exact: true }).click();
  await expect(workspace.getByText("草稿已保存", { exact: true })).toBeVisible();
  const draft = (
    await rpc(page.request, "research.project.list", { after: "", limit: 30 })
  ).research_project.drafts.find(
    (draft: { definition: { title: string } }) => draft.definition.title === "独立回测研究计划证据",
  );
  expect(draft).toBeTruthy();
  const stopped = (await rpc(page.request, "runtime.snapshot")).nodes
    .find((node: { id: string }) => node.id === "local")
    .health.services.find((service: { id: string }) => service.id === dataService);
  expect(stopped.desired_running).toBe(false);
  await workspace.getByLabel("已核对上方原文，固定后保留原标准", { exact: true }).check();
  await workspace.getByRole("button", { name: "固定计划", exact: true }).click();
  await expect(
    workspace.getByRole("button", { name: "按此计划配置回测实验", exact: true }),
  ).toBeVisible();
  const originalPlan = (
    await rpc(page.request, "research.plan.list", { project_id: draft.id, after: "", limit: 30 })
  ).research_project.plans[0];
  expect(originalPlan.exploratory).toBe(false);
  await seedDataset(
    page.request,
    Array.from({ length: 40 }, (_, i) => 100 + i + (i % 3)),
    "backtest-project-fixed-plan",
  );
  await workspace.getByRole("button", { name: "按此计划配置回测实验", exact: true }).click();
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(workspace.getByRole("combobox", { name: "选择固定计划", exact: true })).toHaveValue(
    originalPlan.id,
  );
  await workspace.getByLabel("快均线", { exact: true }).fill("1");
  await workspace.getByLabel("慢均线", { exact: true }).fill("3");
  await workspace.getByLabel("目标手数", { exact: true }).fill("1");
  for (const [label, value] of [
    ["初始资金", "10000"],
    ["每手保证金", "100"],
    ["滑点（跳）", "0"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "1"],
    ["总持仓量上限", "1"],
    ["在途委托数上限", "4"],
  ])
    await workspace.getByLabel(label, { exact: true }).fill(value);
  await workspace.getByRole("button", { name: "下一步", exact: true }).click();
  await workspace.getByRole("button", { name: "开始回测", exact: true }).click();
  let taskId = "";
  await expect
    .poll(
      async () => {
        const taskPage = (
          await rpc(page.request, "research.project.tasks", {
            project_id: draft.id,
            before_sequence: 0,
            limit: 30,
          })
        ).project_tasks;
        const task = taskPage.tasks.find(
          (task: { research_plan?: { plan_id: string } }) =>
            task.research_plan?.plan_id === originalPlan.id,
        );
        if (task) taskId = task.id;
        return task?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const original = (await taskRpc(page.request, "research", "task.result.read", { id: taskId }))
    .task_result;
  expect(original.task.research_plan).toMatchObject({
    project_id: draft.id,
    plan_id: originalPlan.id,
    definition_digest: originalPlan.definition_digest,
    owner: originalPlan.owner,
  });
  expect(original.task.program_artifact).toMatch(/^[0-9a-f]{64}$/);
  expect(original.kind).toBe("backtest");
  expect(original.result.account.equity).toBeTruthy();
  expect(original.result.account.fees).toBeTruthy();
  await openView(page, "projects");
  await workspace.getByRole("button", { name: "独立回测研究计划证据", exact: true }).click();
  await workspace
    .getByLabel("支持标准", { exact: true })
    .fill("后来修订的支持标准，不覆盖旧实验。");
  await workspace.getByRole("button", { name: "保存研究草稿", exact: true }).click();
  await expect(workspace.getByText("草稿已保存", { exact: true })).toBeVisible();
  await workspace.getByLabel("前一计划", { exact: true }).selectOption(originalPlan.id);
  await workspace.getByLabel("修订理由", { exact: true }).fill("补充标准，保留旧计划与原结果。");
  await workspace.getByLabel("已核对上方原文，固定后保留原标准", { exact: true }).check();
  await workspace.getByRole("button", { name: "固定计划", exact: true }).click();
  await expect
    .poll(
      async () =>
        (
          await rpc(page.request, "research.plan.list", {
            project_id: draft.id,
            after: "",
            limit: 30,
          })
        ).research_project.plans.length,
    )
    .toBe(2);
  const second = (
    await rpc(page.request, "research.plan.list", { project_id: draft.id, after: "", limit: 30 })
  ).research_project.plans.find((plan: { id: string }) => plan.id !== originalPlan.id);
  expect(second.parent_plan_id).toBe(originalPlan.id);
  expect(second.definition.support).not.toBe(originalPlan.definition.support);
  expect(
    (await rpc(page.request, "research.plan.read", { id: originalPlan.id })).research_project.plan,
  ).toEqual(originalPlan);
  await workspace
    .getByRole("button", { name: `${second.id} · 已记录支持与否定标准`, exact: true })
    .click();
  const manual = workspace.getByRole("region", { name: "人工研究结论", exact: true });
  await manual.getByLabel("正式研究任务身份", { exact: true }).fill(taskId);
  await manual.getByRole("button", { name: "读取待审阅正式结果", exact: true }).click();
  await expect(manual.getByRole("status")).toHaveText("回溯关联：此任务接纳时未固定所选计划");
  expect(
    (
      await rpc(page.request, "research.conclusion.list", {
        project_id: draft.id,
        after: "",
        limit: 30,
      })
    ).research_project.conclusions,
  ).toEqual([]);
  await expect(manual.getByRole("button", { name: "追加人工结论", exact: true })).toBeDisabled();
  await manual.getByLabel("明确人工判断", { exact: true }).selectOption("insufficient");
  await manual
    .getByLabel("人工判断理由", { exact: true })
    .fill("人工回溯审阅：合成样本不足以建立市场结论。");
  await manual.getByLabel("已核对精确计划和正式证据，明确保存人工判断", { exact: true }).check();
  await manual.getByRole("button", { name: "追加人工结论", exact: true }).click();
  await expect(
    manual.getByText("人工回溯审阅：合成样本不足以建立市场结论。", { exact: true }),
  ).toBeVisible();
  const retrospective = (
    await rpc(page.request, "research.conclusion.list", {
      project_id: draft.id,
      after: "",
      limit: 30,
    })
  ).research_project.conclusions[0];
  expect(retrospective.retrospective).toBe(true);
  expect(retrospective.declaration.plan.plan_id).toBe(second.id);
  expect(retrospective.declaration.result.version).toBe(original.task.result_digest);
  expect(retrospective.program_artifact).toBe(original.task.program_artifact);
  await workspace
    .getByRole("button", { name: `${originalPlan.id} · 已记录支持与否定标准`, exact: true })
    .click();
  await manual.getByLabel("正式研究任务身份", { exact: true }).fill(taskId);
  await manual.getByRole("button", { name: "读取待审阅正式结果", exact: true }).click();
  await expect(manual.getByRole("status")).toHaveText(
    "原任务接纳时已固定所选计划；不证明未提前见过样本",
  );
  await manual.getByLabel("明确人工判断", { exact: true }).selectOption("insufficient");
  await manual
    .getByLabel("人工判断理由", { exact: true })
    .fill("人工原计划审阅：结果不自动证明假设成立。");
  await manual.getByLabel("已核对精确计划和正式证据，明确保存人工判断", { exact: true }).check();
  await manual.getByRole("button", { name: "追加人工结论", exact: true }).click();
  await expect(
    manual.getByText("人工原计划审阅：结果不自动证明假设成立。", { exact: true }),
  ).toBeVisible();
  const human = (
    await rpc(page.request, "research.conclusion.list", {
      project_id: draft.id,
      after: "",
      limit: 30,
    })
  ).research_project.conclusions.find((row: { retrospective: boolean }) => !row.retrospective);
  expect(human.declaration.plan.plan_id).toBe(originalPlan.id);
  await manual.getByLabel("修订前条结论", { exact: true }).selectOption(human.declaration.id);
  await manual.getByLabel("明确人工判断", { exact: true }).selectOption("insufficient");
  await manual
    .getByLabel("人工判断理由", { exact: true })
    .fill("补充人工结论：仍需独立市场样本，保留前条判断。");
  await manual.getByLabel("结论修订理由", { exact: true }).fill("明确样本限制，不重写前条记录。");
  await manual.getByLabel("已核对精确计划和正式证据，明确保存人工判断", { exact: true }).check();
  await manual.getByRole("button", { name: "追加人工结论", exact: true }).click();
  await expect(
    manual.getByText("补充人工结论：仍需独立市场样本，保留前条判断。", { exact: true }),
  ).toBeVisible();
  expect(
    (await rpc(page.request, "research.conclusion.read", { id: human.declaration.id }))
      .research_project.conclusion,
  ).toEqual(human);
  const conclusions = (
    await rpc(page.request, "research.conclusion.list", {
      project_id: draft.id,
      after: "",
      limit: 30,
    })
  ).research_project.conclusions;
  expect(conclusions).toHaveLength(3);
  const service = (await rpc(page.request, "runtime.snapshot")).research_service.service;
  await rpc(page.request, "node.action", { id: "local", service: dataService, action: "stop" });
  await rpc(page.request, "node.action", { id: "local", service, action: "restart" });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.find((item: { id: string }) => item.id === service).health,
      { timeout: 20000 },
    )
    .toBe("ready");
  await openResearch(page.request);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: taskId })).task_result,
  ).toEqual(original);
  await page.reload();
  await openView(page, "projects");
  await workspace.getByRole("button", { name: "独立回测研究计划证据", exact: true }).click();
  const records = workspace.getByRole("region", { name: "项目实验记录", exact: true });
  await expect(records).toContainText(taskId);
  await workspace
    .getByRole("button", { name: `${originalPlan.id} · 已记录支持与否定标准`, exact: true })
    .click();
  await expect(
    manual.getByText("人工原计划审阅：结果不自动证明假设成立。", { exact: true }),
  ).toBeVisible();
  expect(
    (
      await rpc(page.request, "research.conclusion.list", {
        project_id: draft.id,
        after: "",
        limit: 30,
      })
    ).research_project.conclusions,
  ).toEqual(conclusions);
  await records.getByRole("button", { name: "查看结果", exact: true }).click();
  const originalDetails = workspace.getByText("任务原研究计划", { exact: true });
  await originalDetails.click();
  await expect(workspace.getByRole("region", { name: "回测结果", exact: true })).toBeVisible();
  await expect(originalDetails.locator("..")).toContainText(originalPlan.definition.support);
  await expect(originalDetails.locator("..")).not.toContainText(second.definition.support);
  await page.goto("/?screen=settings&category=resources");
  const catalogue = page.getByRole("region", { name: "业务资产目录", exact: true });
  await catalogue.getByLabel("资产所属域").selectOption("research");
  for (const [kind, count] of [
    ["research_project", 1],
    ["research_plan", 2],
    ["research_conclusion", 3],
  ] as const) {
    await catalogue.getByLabel("资产类别").selectOption(kind);
    await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
    const rows = catalogue.locator("tbody tr");
    await expect(rows).toHaveCount(count);
    await expect(rows.first()).toContainText("在研究项目入口读取原计划与人工结论");
    await expect(rows.first().getByRole("button")).toHaveCount(0);
    await expect(rows.first()).toContainText(
      kind === "research_project" ? "研究草稿（可修订）" : "已固定（只读）",
    );
    const listed = (
      await rpc(page.request, "resource.catalogue", { domain: "research", kind, after: "" })
    ).resources;
    expect(listed.items).toHaveLength(count);
    for (const row of listed.items) {
      expect(row.object.node).toBe(originalPlan.owner.node_id);
      expect(row.object.service).toBe(originalPlan.owner.service_id);
      expect(row.object.kind).toBe(kind);
      expect(row.relative_location).toBe("tasks.sqlite");
    }
  }
  expect(
    (
      await rpc(page.request, "research.conclusion.list", {
        project_id: draft.id,
        after: "",
        limit: 30,
      })
    ).research_project.conclusions,
  ).toEqual(conclusions);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: taskId })).task_result,
  ).toEqual(original);
});
