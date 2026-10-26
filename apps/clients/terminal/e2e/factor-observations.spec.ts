import { test, expect } from "./test";
import { rpc, seedDataset, seedHistory, taskRpc, openResearch } from "./dataset-fixture";
import { openView } from "./navigation-helper";

test("retained factor observations distinguish zero and unknown labels and survive Data offline plus Research restart", async ({
  page,
}) => {
  test.setTimeout(90000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await seedDataset(
    page.request,
    Array.from({ length: 40 }, (_, i) => (i < 3 ? 100 : 100 + i + (i % 3))),
    "factor-observation-evidence",
  );
  await page.goto("/");
  await openView(page, "factor");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(workspace.getByText("研究服务未连接", { exact: true })).toHaveCount(0);
  await workspace.getByLabel("回看 K 线数", { exact: true }).fill("2");
  await workspace.getByLabel("未来收益 K 线数", { exact: true }).fill("2");
  await workspace.getByLabel("保存逐行特征与标签证据", { exact: true }).check();
  const before = new Set(
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.map(
      (t: { id: string }) => t.id,
    ),
  );
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const record = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (t: { id: string; kind: string }) => t.kind === "factor" && !before.has(t.id),
        );
        if (record) id = record.id;
        return record?.state;
      },
      { timeout: 20000 },
    )
    .toBe("succeeded");
  const record = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: id });
  await record.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "因子结果", exact: true });
  await expect(result.getByRole("region", { name: "历史可知性", exact: true })).toContainText(
    "不能证明历史当时可交易",
  );
  await result.getByText("逐行特征与标签证据", { exact: true }).click();
  const rows = result
    .getByRole("table", { name: "逐行特征与标签证据", exact: true })
    .locator("tbody tr");
  await expect(rows).toHaveCount(40);
  await expect(rows.nth(0)).toContainText("评价预热未完成");
  await expect(rows.nth(0).getByRole("cell").nth(4)).toHaveText("0");
  await expect(rows.nth(2).getByRole("cell").nth(2)).toHaveText("0");
  await expect(rows.nth(39).getByRole("cell").nth(4)).toHaveText("未来标签尚未知");
  const original = (await taskRpc(page.request, "research", "task.result.read", { id }))
    .task_result;
  expect(original.experiment.retain_observations).toBe(true);
  expect(original.task.program_artifact).toMatch(/^[0-9a-f]{64}$/);
  expect(original.result.observations).toHaveLength(40);
  expect(original.result.observations[2].value).toBe(0);
  expect(original.result.observations[39].forward_return).toBeNull();
  expect(original.result.observations[39].label_end_index).toBeNull();
  await workspace.getByLabel("回看 K 线数", { exact: true }).fill("2,3");
  await expect(workspace.getByRole("button", { name: "开始分析", exact: true })).toBeDisabled();
  await expect(workspace).toContainText("当前配置不支持逐行证据");
  await workspace.getByLabel("回看 K 线数", { exact: true }).fill("2");
  await expect(workspace.getByLabel("保存逐行特征与标签证据", { exact: true })).toBeChecked();
  const state = await rpc(page.request, "runtime.snapshot");
  const dataService = state.data.service;
  const researchService = state.research_service.service;
  await rpc(page.request, "node.action", { id: "local", service: dataService, action: "stop" });
  await rpc(page.request, "node.action", {
    id: "local",
    service: researchService,
    action: "restart",
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((n: { id: string }) => n.id === "local")
          .health.services.find((s: { id: string }) => s.id === researchService).health,
      { timeout: 20000 },
    )
    .toBe("ready");
  await openResearch(page.request);
  const replay = (await taskRpc(page.request, "research", "task.result.read", { id })).task_result;
  expect(replay.task.result_digest).toBe(original.task.result_digest);
  expect(replay.task.program_artifact).toBe(original.task.program_artifact);
  expect(replay.result).toEqual(original.result);
  expect(replay.experiment).toEqual(original.experiment);
  const offline = (await rpc(page.request, "runtime.snapshot")).nodes
    .find((n: { id: string }) => n.id === "local")
    .health.services.find((s: { id: string }) => s.id === dataService);
  expect(offline.desired_running).toBe(false);
  await page.reload();
  await openView(page, "factor");
  await record.getByRole("button", { name: "查看结果", exact: true }).click();
  await result.getByText("逐行特征与标签证据", { exact: true }).click();
  await expect(rows).toHaveCount(40);
  await expect(rows.nth(39).getByRole("cell").nth(4)).toHaveText("未来标签尚未知");
});

test("fixed daily factor parameter derivation retains original row evidence and programme", async ({
  page,
}) => {
  test.setTimeout(90000);
  await page.goto("/");
  const days = Array.from({ length: 50 }, (_, i) =>
    new Date(Date.UTC(2026, 7, 20 + i)).toISOString().slice(0, 10),
  ).filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()));
  await seedHistory(page.request, [100], "retained-daily-derivation", {
    product: "rb",
    tablesOnly: true,
    dailyDays: days,
  });
  await page.reload();
  await openView(page, "factor");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByRole("button", { name: "全市场数据表", exact: true }).click();
  await workspace.getByLabel("品种或合约", { exact: true }).fill("SHFE/rb/2026-10");
  await workspace.getByRole("button", { name: "添加", exact: true }).click();
  await workspace.getByLabel("起始日期", { exact: true }).fill(days[0]);
  await workspace.getByLabel("结束日期", { exact: true }).fill(days.at(-1)!);
  await workspace.getByLabel("回看日线数", { exact: true }).fill("2");
  await workspace.getByLabel("未来日线数", { exact: true }).fill("1");
  await workspace.getByLabel("保存逐行特征与标签证据", { exact: true }).check();
  const before = new Set(
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.map(
      (t: { id: string }) => t.id,
    ),
  );
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const record = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (t: { id: string; kind: string }) => t.kind === "factor" && !before.has(t.id),
        );
        if (record) id = record.id;
        return record?.state;
      },
      { timeout: 20000 },
    )
    .toBe("succeeded");
  const row = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: id });
  await row.getByRole("button", { name: "查看结果", exact: true }).click();
  const original = (await taskRpc(page.request, "research", "task.result.read", { id }))
    .task_result;
  expect(original.experiment.retain_observations).toBe(true);
  expect(original.result.observations).toHaveLength(days.length);
  const reuse = workspace.getByRole("region", { name: "按此日线实验创建", exact: true });
  await reuse.getByRole("button", { name: "修改回看窗口并派生", exact: true }).click();
  await reuse.getByLabel("派生回看日线数", { exact: true }).fill("3");
  await reuse.getByLabel("沿用原固定快照", { exact: true }).check();
  await reuse.getByRole("button", { name: "核对参数派生", exact: true }).click();
  await reuse.getByRole("button", { name: "确认创建派生因子任务", exact: true }).click();
  const childId = (await reuse.getByRole("status").locator("code").textContent())!;
  expect(childId).not.toBe(id);
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (t: { id: string }) => t.id === childId,
        )?.state,
      { timeout: 20000 },
    )
    .toBe("succeeded");
  const child = (await taskRpc(page.request, "research", "task.result.read", { id: childId }))
    .task_result;
  expect(child.experiment.retain_observations).toBe(true);
  expect(child.experiment.lookbacks).toEqual([3]);
  expect(child.experiment.dataset_revision).toBe(original.experiment.dataset_revision);
  expect(child.task.program_artifact).toBe(original.task.program_artifact);
  expect(child.result.observations).toHaveLength(days.length);
  expect(child.result.observations[2].value).toBeNull();
  expect(child.result.observations[3].value).not.toBeNull();
  const unchanged = (await taskRpc(page.request, "research", "task.result.read", { id }))
    .task_result;
  expect(unchanged).toEqual(original);
});

test("retained holdout evidence distinguishes known crossed labels from unknown future and survives restart", async ({
  page,
}) => {
  test.setTimeout(90000);
  await seedDataset(
    page.request,
    Array.from({ length: 100 }, (_, i) => 100 + i + (i % 7)),
    "retained-holdout-evidence",
  );
  await page.goto("/");
  await openView(page, "factor");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace.getByLabel("回看 K 线数", { exact: true }).fill("2");
  await workspace.getByLabel("未来收益 K 线数", { exact: true }).fill("2");
  await workspace.getByLabel("评价方式", { exact: true }).selectOption("holdout");
  await workspace.getByLabel("前段 K 线数", { exact: true }).fill("50");
  await workspace.getByLabel("保存逐行特征与标签证据", { exact: true }).check();
  const before = new Set(
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.map(
      (t: { id: string }) => t.id,
    ),
  );
  await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
  let id = "";
  await expect
    .poll(
      async () => {
        const record = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (t: { id: string; kind: string }) => t.kind === "factor" && !before.has(t.id),
        );
        if (record) id = record.id;
        return record?.state;
      },
      { timeout: 20000 },
    )
    .toBe("succeeded");
  const record = workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: id });
  await record.getByRole("button", { name: "查看结果", exact: true }).click();
  const result = workspace.getByRole("region", { name: "因子结果", exact: true });
  await expect(result).toContainText("剔除跨界标签: 2");
  await result.getByText("逐行特征与标签证据", { exact: true }).click();
  const rows = result
    .getByRole("table", { name: "逐行特征与标签证据", exact: true })
    .locator("tbody tr");
  await expect(rows).toHaveCount(50);
  await expect(rows.nth(48)).toContainText("标签跨开发边界，排除评价");
  await expect(rows.nth(48).getByRole("cell").nth(6)).toHaveText("开发段");
  await expect(rows.nth(48).getByRole("cell").nth(4)).not.toHaveText("未来标签尚未知");
  await result.getByRole("button", { name: "下一页逐行证据", exact: true }).click();
  await expect(rows.nth(0).getByRole("cell").nth(6)).toHaveText("留出段");
  await expect(rows.nth(0)).toContainText("参与评价");
  await expect(rows.nth(49).getByRole("cell").nth(4)).toHaveText("未来标签尚未知");
  const original = (await taskRpc(page.request, "research", "task.result.read", { id }))
    .task_result;
  expect(original.result.observations[48]).toMatchObject({
    role: "development",
    label_crosses_boundary: true,
    included: false,
    label_end_index: 50,
  });
  expect(original.result.observations[48].forward_return).not.toBeNull();
  expect(original.result.observations[50]).toMatchObject({
    role: "holdout",
    label_crosses_boundary: false,
    included: true,
  });
  expect(original.result.observations[99].forward_return).toBeNull();
  const service = (await rpc(page.request, "runtime.snapshot")).research_service.service;
  await rpc(page.request, "node.action", { id: "local", service, action: "restart" });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((n: { id: string }) => n.id === "local")
          .health.services.find((s: { id: string }) => s.id === service).health,
      { timeout: 20000 },
    )
    .toBe("ready");
  await openResearch(page.request);
  expect((await taskRpc(page.request, "research", "task.result.read", { id })).task_result).toEqual(
    original,
  );
});

for (const mode of ["search", "rolling"] as const)
  test(`retained ${mode} evidence keeps selected definition and each training boundary after restart`, async ({
    page,
  }) => {
    test.setTimeout(90000);
    const rolling = mode === "rolling";
    await seedDataset(
      page.request,
      Array.from({ length: rolling ? 160 : 100 }, (_, i) => 100 + i + ((i * i) % 17)),
      `retained-${mode}-evidence`,
    );
    await page.goto("/");
    await openView(page, "factor");
    const workspace = page.getByRole("region", { name: "期货研究", exact: true });
    await workspace.getByLabel("回看 K 线数", { exact: true }).fill("2,5,10");
    await workspace.getByLabel("未来收益 K 线数", { exact: true }).fill("2");
    await workspace
      .getByLabel("评价方式", { exact: true })
      .selectOption(rolling ? "walk_forward" : "holdout");
    if (rolling) {
      await workspace.getByLabel("训练 K 线数", { exact: true }).fill("80");
      await workspace.getByLabel("每轮验证 K 线数", { exact: true }).fill("40");
    } else await workspace.getByLabel("前段 K 线数", { exact: true }).fill("50");
    await workspace.getByLabel("保存逐行特征与标签证据", { exact: true }).check();
    const before = new Set(
      (await rpc(page.request, "runtime.snapshot")).research_service.tasks.map(
        (t: { id: string }) => t.id,
      ),
    );
    await workspace.getByRole("button", { name: "开始分析", exact: true }).click();
    let id = "";
    await expect
      .poll(
        async () => {
          const record = (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
            (t: { id: string; kind: string }) => t.kind === "factor" && !before.has(t.id),
          );
          if (record) id = record.id;
          return record?.state;
        },
        { timeout: 20000 },
      )
      .toBe("succeeded");
    const record = workspace
      .getByRole("region", { name: "研究任务", exact: true })
      .getByRole("row")
      .filter({ hasText: id });
    await record.getByRole("button", { name: "查看结果", exact: true }).click();
    const result = workspace.getByRole("region", { name: "因子结果", exact: true });
    await expect(result).toContainText("事后选参诊断");
    const original = (await taskRpc(page.request, "research", "task.result.read", { id }))
      .task_result;
    expect(original.task.program_artifact).toMatch(/^[0-9a-f]{64}$/);
    if (rolling) {
      expect(original.result.observations).toBeUndefined();
      expect(original.result.folds).toHaveLength(2);
      for (const [i, fold] of original.result.folds.entries()) {
        expect(fold.observations).toHaveLength(120);
        expect(fold.observations[0].event_index).toBe(i * 40);
        expect(fold.observations[78]).toMatchObject({
          role: "development",
          label_crosses_boundary: true,
          included: false,
        });
        expect(fold.observations[78].forward_return).not.toBeNull();
        expect(fold.observations[80]).toMatchObject({ role: "holdout", included: true });
        expect(fold.observations[119].forward_return).toBeNull();
      }
      await result.getByText("逐轮选参证据", { exact: true }).click();
      const section = result.getByRole("heading", { name: "轮次 1", exact: true }).locator("..");
      await section.getByText("逐行特征与标签证据", { exact: true }).click();
      const rows = section
        .getByRole("table", { name: "逐行特征与标签证据", exact: true })
        .locator("tbody tr");
      await expect(rows).toHaveCount(50);
      await section.getByRole("button", { name: "下一页逐行证据", exact: true }).click();
      await expect(rows.nth(28)).toContainText("标签跨开发边界，排除评价");
      await expect(rows.nth(30).getByRole("cell").nth(6)).toHaveText("留出段");
      await section.getByRole("button", { name: "下一页逐行证据", exact: true }).click();
      await expect(rows).toHaveCount(20);
      await expect(rows.nth(19).getByRole("cell").nth(4)).toHaveText("未来标签尚未知");
    } else {
      expect(original.result.candidates).toHaveLength(3);
      expect(original.result.observations).toHaveLength(100);
      for (let i = 0; i < 10; ++i)
        expect(original.result.observations[i].evaluation_warmup).toBe(true);
      expect(original.result.observations[48]).toMatchObject({
        role: "development",
        label_crosses_boundary: true,
        included: false,
      });
      await result.getByText("逐行特征与标签证据", { exact: true }).click();
      await expect(
        result
          .getByRole("table", { name: "逐行特征与标签证据", exact: true })
          .locator("tbody tr")
          .nth(48),
      ).toContainText("标签跨开发边界，排除评价");
    }
    const service = (await rpc(page.request, "runtime.snapshot")).research_service.service;
    await rpc(page.request, "node.action", { id: "local", service, action: "restart" });
    await expect
      .poll(
        async () =>
          (await rpc(page.request, "runtime.snapshot")).nodes
            .find((n: { id: string }) => n.id === "local")
            .health.services.find((s: { id: string }) => s.id === service).health,
        { timeout: 20000 },
      )
      .toBe("ready");
    await openResearch(page.request);
    expect(
      (await taskRpc(page.request, "research", "task.result.read", { id })).task_result,
    ).toEqual(original);
  });
