import { taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { seedHistory, rpc } from "./dataset-fixture";
import { statSync } from "node:fs";
import { join } from "node:path";

test(
  "a downloaded dataset reaches a fixed backtest result without reselecting its source",
  { tag: "@journey" },
  async ({ page }) => {
    await page.goto("/");
    await expect(page.locator(".workspace-tabs")).toBeVisible();
    const selected = await seedHistory(
      page.request,
      [100, 101, 102, 101, 104, 103, 102, 103],
      "flow-source",
    );
    await page.reload();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await openView(page, "records");
    await page
      .locator(`[data-dataset-id="${selected.source_dataset_ids[0]}"]`)
      .getByRole("button", { name: "用于回测" })
      .click();
    await expect(page.getByRole("heading", { name: "新建回测", exact: true })).toBeVisible();
    // A minute download opens the form that takes one.
    const minute = page.getByRole("form", { name: "分钟线合约", exact: true });
    await expect(minute.getByLabel("K 线来源", { exact: true })).toHaveValue(
      selected.source_dataset_ids[0],
    );
    // Nothing to choose for settlement prices: they are the daily table's.
    await expect(
      page.getByText(
        /^结算价和交易日取自全市场日线表：SHFE 已入库 .* 读取，之后入库的数据不影响这次选择。$/,
      ),
    ).toBeVisible();
    await minute.getByLabel("开始交易日", { exact: true }).fill("2026-09-25");
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await openView(page, "backtest");
    await expect(minute.getByLabel("K 线来源", { exact: true })).toHaveValue(
      selected.source_dataset_ids[0],
    );
    await expect(minute.getByLabel("开始交易日", { exact: true })).toHaveValue("2026-09-25");
    await minute.getByRole("button", { name: "使用此数据集", exact: true }).click();
    await expect(page.getByRole("list", { name: "已选合约" })).toContainText("8 根 · 1 个交易日");
    await page.getByRole("button", { name: "下一步", exact: true }).click();
    for (const [label, value] of [
      ["快均线", "1"],
      ["慢均线", "3"],
      ["目标手数", "1"],
      ["初始资金", "10000"],
      ["每手保证金", "100"],
      ["滑点（跳）", "0"],
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
      ["单笔数量上限", "100"],
      ["总持仓量上限", "100"],
      ["在途委托数上限", "100"],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "下一步", exact: true }).click();
    await page.getByRole("button", { name: "开始回测", exact: true }).click();
    const progress = page.getByRole("region", { name: "回测进度", exact: true });
    await progress.getByRole("button", { name: "查看结果" }).click({ timeout: 20000 });
    const result = page.getByRole("region", { name: "回测结果", exact: true });
    await expect(result.getByRole("img", { name: "权益曲线" })).toBeVisible();
    await result.getByText("实验参数", { exact: true }).click();
    await expect(result).toContainText(selected.source_dataset_ids[0]);
    await page.screenshot({
      path: "apps/clients/terminal/test-results/download-backtest-result.png",
    });
    await result.getByRole("button", { name: "查看绩效与归因", exact: true }).click();
    const evaluation = page.getByRole("region", { name: "绩效与归因", exact: true });
    const id = await evaluation.getByLabel("评估来源").inputValue();
    expect(id).toBeTruthy();
    await evaluation.getByRole("button", { name: "读取评估来源" }).click();
    await expect(evaluation.getByRole("region", { name: "绩效", exact: true })).toBeVisible();
    await expect(evaluation).toContainText("未配置无风险收益假设");
    const evidence = (await taskRpc(page.request, "research", "task.result.read", { id }))
      .task_result;
    await expect(evaluation).toContainText(evidence.task.result_digest);
    expect(evidence.task.program_artifact).toMatch(/^[0-9a-f]{64}$/);
    await evaluation.getByRole("button", { name: "查看原始实验" }).click();
    await expect(
      page.locator(".workspace-tabs").getByRole("button", { name: "回测", exact: true }),
    ).toHaveAttribute("aria-current", "page");
    await page
      .getByRole("region", { name: "回测进度", exact: true })
      .getByRole("button", { name: "查看结果", exact: true })
      .click();
    const reuse = page.getByRole("region", { name: "按此回测创建", exact: true });
    await reuse.getByRole("radio", { name: "沿用原固定快照", exact: true }).check();
    await reuse.getByRole("button", { name: "创建独立回测任务", exact: true }).click();
    const child = await reuse.getByRole("status").locator("code").innerText();
    await expect
      .poll(
        async () =>
          (await taskRpc(page.request, "research", "task.retirement.preview", { id: child }))
            .task_retirement.task.state,
        { timeout: 20000 },
      )
      .toBe("succeeded");
    const derivedEvidence = (
      await taskRpc(page.request, "research", "task.result.read", { id: child })
    ).task_result;
    expect(derivedEvidence.task.program_artifact).toBe(evidence.task.program_artifact);
    const referenceState = await rpc(page.request, "runtime.snapshot");
    const researchRoot = referenceState.nodes
      .find((node: { id: string }) => node.id === "local")
      .health.services.find(
        (service: { id: string }) => service.id === referenceState.research_service.service,
      ).directory;
    expect(researchRoot.startsWith(process.env.ASTERION_NODE_DIRECTORY + "/")).toBe(true);
    expect(
      statSync(join(researchRoot, "programs", evidence.task.program_artifact)).size,
    ).toBeGreaterThan(0);
    const originalFile = statSync(join(researchRoot, id, "input.pb"));
    const referencedFile = statSync(join(researchRoot, child, "input.pb"));
    expect(referencedFile.dev).toBe(originalFile.dev);
    expect(referencedFile.ino).toBe(originalFile.ino);
    expect(referencedFile.nlink).toBeGreaterThanOrEqual(2);
    const held = (
      await rpc(page.request, "data.history.usage", { id: selected.source_dataset_ids[0] })
    ).history_usage;
    expect(held.consumers).toHaveLength(2);
    await page.getByRole("button", { name: "返回回测记录", exact: true }).click();
    const childRecord = page
      .getByRole("region", { name: "研究任务", exact: true })
      .getByRole("row")
      .filter({ has: page.locator("td > details > code").filter({ hasText: child }) });
    await childRecord.getByRole("button", { name: "维护研究记录", exact: true }).click();
    const childMaintenance = childRecord.getByRole("region", { name: "研究记录维护", exact: true });
    await expect(childMaintenance).toContainText("可退役");
    const parentBlocked = await taskRpc(page.request, "research", "task.retirement.preview", {
      id,
    });
    expect(parentBlocked.task_retirement.state).toBe("blocked");
    expect(parentBlocked.task_retirement.consumers).toContain(child);
    await childMaintenance
      .getByRole("checkbox", { name: "确认退役此记录，禁止重试和新增派生" })
      .check();
    await childMaintenance.getByRole("button", { name: "确认退役", exact: true }).click();
    await expect(childMaintenance).toContainText("Data 已确认解除源保留");
    expect(
      (await rpc(page.request, "data.history.usage", { id: selected.source_dataset_ids[0] }))
        .history_usage.consumers,
    ).toHaveLength(1);
    const record = page
      .getByRole("region", { name: "研究任务", exact: true })
      .getByRole("row")
      .filter({ has: page.locator("td > details > code").filter({ hasText: id }) });
    await record.getByRole("button", { name: "维护研究记录", exact: true }).click();
    const maintenance = record.getByRole("region", { name: "研究记录维护", exact: true });
    await expect(maintenance).toContainText("可退役");
    await expect(maintenance.getByRole("button", { name: "确认退役", exact: true })).toBeDisabled();
    await maintenance.getByRole("checkbox", { name: "确认退役此记录，禁止重试和新增派生" }).check();
    await maintenance.getByRole("button", { name: "确认退役", exact: true }).click();
    await expect(maintenance).toContainText("Data 已确认解除源保留");
    const retired = await taskRpc(page.request, "research", "task.retirement.preview", { id });
    expect(retired.task_retirement.state).toBe("retired");
    expect(retired.task_retirement.source_release_confirmed).toBe(true);
    const usage = await rpc(page.request, "data.history.usage", {
      id: selected.source_dataset_ids[0],
    });
    expect(usage.history_usage.consumers).toHaveLength(0);
    const retained = (await taskRpc(page.request, "research", "task.result.read", { id }))
      .task_result;
    expect(retained.task.result_digest).toBe(evidence.task.result_digest);
    expect(retained.task.program_artifact).toBe(evidence.task.program_artifact);
    expect(retained.task.retired).toBe(true);
    await record.getByRole("button", { name: "查看结果", exact: true }).click();
    await expect(page.getByText("原记录已退役，不能新增派生实验").first()).toBeVisible();
  },
);

test("data handoff stays bound to its service and never reuses another contract's units", async ({
  page,
}) => {
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  const one = await seedHistory(page.request, [100, 101, 102], "handoff-rb");
  const two = await seedHistory(page.request, [100, 101, 102], "handoff-hc", {
    product: "hc",
    keep: true,
  });
  await rpc(page.request, "data.dataset.select", two);
  let identity = "handoff-service";
  await page.route("**/__asterion/api", async route => {
    if (route.request().postDataJSON().method !== "runtime.snapshot") return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    if (body.result?.data) body.result.data.connection_id = identity;
    await route.fulfill({ response, json: body });
  });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  await page
    .locator(`[data-dataset-id="${one.source_dataset_ids[0]}"]`)
    .getByRole("button", { name: "用于回测" })
    .click();
  await expect(page.getByRole("button", { name: "下一步", exact: true })).toBeDisabled();
  const minute = page.getByRole("form", { name: "分钟线合约", exact: true });
  await minute.getByLabel("K 线来源", { exact: true }).selectOption(two.source_dataset_ids[0]);
  identity = "other-service-with-same-task-ids";
  await expect(
    page.getByText("数据版本不属于当前数据服务，请重新选择数据。", { exact: true }),
  ).toBeVisible();
  await expect(minute.getByLabel("K 线来源", { exact: true })).toHaveValue("");
  await expect(minute.getByRole("button", { name: "加入组合", exact: true })).toBeDisabled();
  await expect(page.getByRole("button", { name: "下一步", exact: true })).toBeDisabled();
  // A version of the service now connected can be chosen again.
  await minute.getByLabel("K 线来源", { exact: true }).selectOption(one.source_dataset_ids[0]);
  await expect(minute.getByLabel("K 线来源", { exact: true })).toHaveValue(
    one.source_dataset_ids[0],
  );
});

test("data/task access prepares a stopped local service once and does not replace an offline remote connection", async ({
  page,
}) => {
  await page.goto("/");
  await openView(page, "trading");
  const opened = await rpc(page.request, "node.research.local.open");
  await rpc(page.request, "node.action", {
    id: "local",
    service: opened.research_service.service,
    action: "stop",
  });
  let offline = true,
    remote = false,
    starts = 0,
    reads = 0;
  await page.route("**/__asterion/api", async route => {
    const method = route.request().postDataJSON().method;
    if (method === "node.research.local.open") {
      starts++;
      offline = false;
    }
    if (!["runtime.snapshot", "node.research.local.open"].includes(method)) return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    if (body.result?.research_service) {
      reads++;
      if (offline) body.result.research_service.online = false;
      body.result.research_service.remote = remote;
    }
    await route.fulfill({ response, json: body });
  });
  await expect.poll(() => reads).toBeGreaterThan(0);
  await openView(page, "backtest");
  await expect.poll(() => starts).toBe(1);
  await expect(page.getByRole("button", { name: "新建回测", exact: true })).toBeEnabled();
  await openView(page, "trading");
  remote = true;
  offline = true;
  await expect.poll(() => reads).toBeGreaterThan(0);
  await openView(page, "backtest");
  await expect(
    page.getByText("远程数据或研究连接已断开，当前运行位置保持不变。", { exact: true }),
  ).toBeVisible();
  await expect(page.getByRole("button", { name: "新建回测", exact: true })).toBeDisabled();
  expect(starts).toBe(1);
});

test("a failed preparation waits for explicit retry instead of restarting in a loop", async ({
  page,
}) => {
  await page.goto("/");
  await openView(page, "trading");
  let offline = true,
    attempts = 0,
    reads = 0;
  await page.route("**/__asterion/api", async route => {
    const method = route.request().postDataJSON().method;
    if (method === "node.research.local.open") {
      attempts++;
      if (attempts === 1)
        return route.fulfill({
          json: {
            version: 1,
            error: { code: "unavailable", message: "task service is not connected" },
          },
        });
      offline = false;
    }
    if (!["runtime.snapshot", "node.research.local.open"].includes(method)) return route.continue();
    // Every poll is answered with the whole snapshot, whether or not the core
    // has a new revision. The page takes each as a new state, which is what
    // would start the preparation again if it looped; a poll answered with
    // "unchanged" would tell the page nothing and prove nothing.
    const response = await route.fetch(
      method === "runtime.snapshot" ? { postData: { version: 1, method, params: {} } } : {},
    );
    const body = await response.json();
    if (body.result?.research_service) {
      reads++;
      body.result.research_service.remote = false;
      if (offline) body.result.research_service.online = false;
    }
    await route.fulfill({ response, json: body });
  });
  await expect.poll(() => reads).toBeGreaterThan(0);
  await openView(page, "backtest");
  await expect(page.getByRole("button", { name: "重试准备", exact: true })).toBeVisible();
  const afterFailure = reads;
  await expect.poll(() => reads).toBeGreaterThan(afterFailure + 1);
  expect(attempts).toBe(1);
  await page.getByRole("button", { name: "重试准备", exact: true }).click();
  await expect(page.getByRole("button", { name: "新建回测", exact: true })).toBeEnabled();
  expect(attempts).toBe(2);
});

test("archive versions remain selectable without any download tasks", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".workspace-tabs")).toBeVisible();
  await seedHistory(page.request, [100, 101], "unrelated-contract", { product: "al" });
  // A product no other spec seeds.
  const selected = await seedHistory(page.request, [100, 101, 102, 103], "archive-only", {
    product: "sn",
  });
  await page.route("**/__asterion/api", async route => {
    if (route.request().postDataJSON().method !== "runtime.snapshot") return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    if (body.result?.research_service) body.result.research_service.tasks = [];
    await route.fulfill({ response, json: body });
  });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await openView(page, "records");
  await page
    .locator(`[data-dataset-id="${selected.source_dataset_ids[0]}"]`)
    .getByRole("button", { name: "用于回测" })
    .click();
  const minute = page.getByRole("form", { name: "分钟线合约", exact: true });
  await expect(minute.getByLabel("K 线来源", { exact: true })).toHaveValue(
    selected.source_dataset_ids[0],
  );
  await expect(minute.getByText(/^结算价和交易日取自全市场日线表：SHFE 已入库 /)).toBeVisible();
  await minute.getByRole("button", { name: "使用此数据集", exact: true }).click();
  await expect(page.getByRole("list", { name: "已选合约" })).toContainText("4 根 · 1 个交易日");
  await expect(page.getByRole("button", { name: "下一步", exact: true })).toBeEnabled();
  await page.screenshot({
    path: "apps/clients/terminal/test-results/archive-backtest-factor-selection.png",
  });
});
