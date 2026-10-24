import { taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { execFileSync } from "node:child_process";

test("F10 Research persists finite cost batches without client task dispatch and preserves baseline", async ({
  page,
}) => {
  test.setTimeout(120000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const prices = [100, 102, 106, 110, 104, 98, 94, 100, 107, 112, 103, 95];
  const days = Array.from({ length: 20 }, (_, i) =>
    new Date(Date.UTC(2026, 8, 7 + i)).toISOString().slice(0, 10),
  )
    .filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()))
    .slice(0, prices.length);
  const seeded = await seedHistory(page.request, prices, "f10-daily", {
    tablesOnly: true,
    dailyPrices: true,
    dailyDays: days,
  });
  await rpc(page.request, "data.dataset.daily", {
    contract_id: "SHFE/rb/2026-10",
    daily: seeded.daily,
    begin_day: days[0],
    end_day: days.at(-1)!,
  });
  await rpc(page.request, "backtest.submit", {
    id: "f10-baseline",
    strategies: [{ quantity: "1", sides: "both", rule: { kind: "momentum", lookback: 2 } }],
    holdout_from: "",
    walk_forward: null,
    deposit: "100000",
    max_order_quantity: "1",
    max_gross_quantity: "1",
    max_working_orders: "4",
    contracts: [
      {
        venue: "SHFE",
        symbol: "rb2610",
        slippage_ticks: 0,
        cost_schedule: [
          {
            effective_from: "1970-01-01",
            source: "isolated explicit test cost",
            values: {
              margin_per_lot: "100",
              margin_rate: "0",
              open_fee: "1",
              close_today_fee: "1",
              close_yesterday_fee: "1",
              open_fee_rate: "0",
              close_today_fee_rate: "0",
              close_yesterday_fee_rate: "0",
            },
          },
        ],
      },
    ],
  });
  const tasks = async () => (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
  await expect
    .poll(async () => (await tasks()).find((t: { id: string }) => t.id === "f10-baseline")?.state)
    .toBe("succeeded");
  const original = (
    await taskRpc(page.request, "research", "task.result.read", { id: "f10-baseline" })
  ).task_result;
  expect(original.result.account.fills.length).toBeGreaterThan(0);
  const before = (await tasks()).map((t: { id: string }) => t.id);
  await page.goto("/");
  await openView(page, "backtest");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "f10-baseline" })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  const panel = workspace.getByRole("region", { name: "固定数据成本敏感性", exact: true });
  const input = panel.getByLabel("滑点变体（1–100个整数，以逗号分隔）", { exact: true });
  await input.fill("1,2");
  for (const [label, value] of [
    ["批次名称", "Explicit costs"],
    ["批次用途", "Post hoc sensitivity"],
    ["批次局限", "Already observed original baseline"],
    ["时间预算（秒，必填）", "30"],
  ])
    await panel.getByLabel(label, { exact: true }).fill(value);
  await panel.getByRole("button", { name: "核对滑点变体", exact: true }).click();
  await expect(panel).toContainText("0 → 1, 2");
  const preview = panel.getByLabel("滑点变体确认", { exact: true });
  const identities = preview.getByRole("list", { name: "预览全部成员身份", exact: true });
  await expect(identities.getByRole("listitem")).toHaveCount(2);
  for (const id of await identities.locator("code").allTextContents())
    expect(id).toMatch(/^cost-[a-f0-9-]+$/);
  for (const identity of [
    original.input_sha256,
    original.task.result_digest,
    original.task.program_artifact,
    original.task.risk_artifact,
  ])
    await expect(preview).toContainText(identity);
  await expect(preview).toContainText(`每次原任务事件数：${original.task.total}`);
  await expect(preview).toContainText(`计划事件工作量：${BigInt(original.task.total) * 2n}`);
  await expect(preview).toContainText("未估算运行时长");
  await panel.getByRole("button", { name: "取消滑点变体", exact: true }).click();
  expect((await tasks()).map((t: { id: string }) => t.id)).toEqual(before);
  await input.fill("1,2");
  await panel.getByRole("button", { name: "核对滑点变体", exact: true }).click();
  await panel.getByRole("button", { name: "保存批次（不执行）", exact: true }).click();
  await expect(panel.getByRole("table", { name: "批次全部成员", exact: true })).toBeVisible();
  expect((await tasks()).map((t: { id: string }) => t.id)).toEqual(before);
  const saved = (await rpc(page.request, "research.batch.list", { after: "", limit: 20 }))
    .research_batch.states;
  expect(saved).toHaveLength(1);
  const batch = saved[0].definition.object;
  expect(saved[0].phase).toBe("saved");
  expect(
    saved[0].definition.members.map((m: { slippage_ticks: number }) => m.slippage_ticks),
  ).toEqual([1, 2]);
  // A fresh UI has no scheduling loop or in-memory batch receipt to resume.
  await page.reload();
  await openView(page, "backtest");
  await workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "f10-baseline" })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  await panel.getByRole("button", { name: "读取已有批次", exact: true }).click();
  await panel.getByRole("button", { name: new RegExp(batch.id) }).click();
  await expect(
    panel.getByRole("table", { name: "批次全部成员", exact: true }).locator("tbody tr"),
  ).toHaveCount(2);
  expect((await tasks()).map((t: { id: string }) => t.id)).toEqual(before);
  await panel.getByRole("button", { name: "确认启动批次", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await tasks()).filter(
          (t: { id: string; state: string }) => t.id !== "f10-baseline" && t.state === "succeeded",
        ).length,
    )
    .toBe(2);
  const variants = (await tasks()).filter((t: { id: string }) => t.id !== "f10-baseline");
  for (const task of variants) {
    const result = (await taskRpc(page.request, "research", "task.result.read", { id: task.id }))
      .task_result;
    const ticks = result.experiment.paper.contracts[0].slippage_ticks;
    expect([1, 2]).toContain(ticks);
    const normalized = structuredClone(result.experiment);
    normalized.paper.contracts[0].slippage_ticks = 0;
    expect(normalized).toEqual(original.experiment);
    expect(result.task.metadata.source_id).toBe("f10-baseline");
    expect(result.task.metadata.data_choice).toBe("fixed");
    expect(result.task.metadata.batch).toEqual(batch);
    expect(result.task.program_artifact).toBe(original.task.program_artifact);
    expect(result.task.risk_artifact).toBe(original.task.risk_artifact);
    expect(result.result.account.equity).not.toBe(original.result.account.equity);
  }
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "f10-baseline" }))
      .task_result,
  ).toEqual(original);
  // Member completion precedes the separate durable batch completion decision.
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "research.batch.read", { id: batch.id })).research_batch.state
          .phase,
    )
    .toBe("completed");
  await panel.getByRole("button", { name: "刷新批次状态", exact: true }).click();
  await expect(panel).toContainText("全部成员已结束");
  await panel.getByRole("button", { name: "读取成本敏感性结果", exact: true }).click();
  const table = panel.getByRole("table", { name: "成本敏感性结果", exact: true });
  await expect(table.locator("tbody tr")).toHaveCount(3);
  await expect(panel).toContainText("不排序、不宣称最优");
  const comparison = workspace.getByRole("region", { name: "回测实验比较", exact: true });
  for (const id of ["f10-baseline", variants[0].id])
    await comparison.getByRole("checkbox", { name: new RegExp(id) }).check();
  await comparison.getByRole("button", { name: "读取并比较", exact: true }).click();
  await expect(comparison.getByRole("table", { name: "回测实验对照", exact: true })).toContainText(
    "滑点不同",
  );
  // Fee scenarios preserve original slippage and actually rerun the account.
  await panel
    .getByRole("combobox", { name: "成本情景类型", exact: true })
    .selectOption({ value: "fees" });
  await panel
    .getByLabel("手续费倍率（1–100个精确小数，以逗号分隔）", { exact: true })
    .fill("1.5,2");
  await panel.getByLabel("批次名称", { exact: true }).fill("Explicit commission scenarios");
  await expect(panel.getByRole("button", { name: "核对手续费变体", exact: true })).toBeDisabled();
  for (const [label, value] of [
    ["批次用途", "Explicit original commission perturbations"],
    ["批次局限", "Already observed source; synthetic fees, not independent validation"],
    ["时间预算（秒，必填）", "30"],
  ])
    await panel.getByLabel(label, { exact: true }).fill(value);
  await expect(panel.getByRole("button", { name: "核对手续费变体", exact: true })).toBeEnabled();

  await panel.getByRole("button", { name: "核对手续费变体", exact: true }).click();
  await expect(panel.getByLabel("手续费变体确认", { exact: true })).toContainText("1.5");
  const beforeFees = (await tasks()).map((item: { id: string }) => item.id).sort();
  await panel.getByRole("button", { name: "保存批次（不执行）", exact: true }).click();
  await expect(panel.getByRole("table", { name: "批次全部成员", exact: true })).toContainText(
    "1.5",
  );
  expect((await tasks()).map((item: { id: string }) => item.id).sort()).toEqual(beforeFees);
  const feeState = (
    await rpc(page.request, "research.batch.list", { after: "", limit: 20 })
  ).research_batch.states.find(
    (item: { definition: { name: string } }) =>
      item.definition.name === "Explicit commission scenarios",
  );
  expect(
    feeState.definition.members.map((item: { fee_multiplier: string }) => item.fee_multiplier),
  ).toEqual(["1.5", "2"]);
  expect(
    feeState.definition.members.map((item: { slippage_ticks: number }) => item.slippage_ticks),
  ).toEqual([0, 0]);
  const feeBatch = feeState.definition.object;
  await panel.getByRole("button", { name: "确认启动批次", exact: true }).click();
  // Actual member publication and the owner's durable batch completion are separate.
  await expect
    .poll(
      async () => {
        const reply = (await rpc(page.request, "research.batch.read", { id: feeBatch.id }))
          .research_batch;
        return reply.members.filter((member: { state: string }) => member.state === "succeeded")
          .length;
      },
      { timeout: feeState.definition.time_budget_seconds * 1000 },
    )
    .toBe(2);
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "research.batch.read", { id: feeBatch.id })).research_batch.state
          .phase,
    )
    .toBe("completed");
  const units = (value: string) => {
    const negative = value.startsWith("-");
    const [whole, fraction = ""] = (negative ? value.slice(1) : value).split(".");
    return (negative ? -1n : 1n) * (BigInt(whole) * 100000000n + BigInt(fraction.padEnd(8, "0")));
  };
  for (const member of feeState.definition.members) {
    const actual = (
      await taskRpc(page.request, "research", "task.result.read", { id: member.task_id })
    ).task_result;
    expect(actual.task.state).toBe("succeeded");
    expect(actual.task.metadata.batch).toEqual(feeBatch);
    expect(actual.task.program_artifact).toBe(original.task.program_artifact);
    const normalized = structuredClone(actual.experiment);
    normalized.paper.contracts[0].cost_schedule =
      original.experiment.paper.contracts[0].cost_schedule;
    expect(normalized).toEqual(original.experiment);
    const fills = (values: { quantity: string; price: string; symbol: string }[]) =>
      values.map(fill => [fill.quantity, fill.price, fill.symbol]);
    expect(fills(actual.result.account.fills)).toEqual(fills(original.result.account.fills));
    expect(units(actual.result.account.fees)).toBe(
      (units(original.result.account.fees) * units(member.fee_multiplier)) / 100000000n,
    );
    expect(units(actual.result.account.equity) - units(original.result.account.equity)).toBe(
      units(original.result.account.fees) - units(actual.result.account.fees),
    );
  }
  await panel.getByRole("button", { name: "刷新批次状态", exact: true }).click();
  await panel.getByRole("button", { name: "读取成本敏感性结果", exact: true }).click();
  await expect(table.locator("tbody tr")).toHaveCount(3);
  await expect(table).toContainText("1.5");
  // A real saved-but-unstarted manifest must remain unadmitted after review.
  const idle = (
    await rpc(page.request, "research.batch.save", {
      id: "save-review-unstarted",
      definition: {
        id: "review-unstarted",
        name: "Declared but never started",
        purpose: "Observe missing evidence without dispatch",
        limitations: "Synthetic post-hoc full account; independence is not established",
        source: {
          ...batch,
          kind: "backtest_result",
          id: "f10-baseline",
          version: original.task.result_digest,
        },
        members: [
          { task_id: "review-unstarted-1", slippage_ticks: 1 },
          { task_id: "review-unstarted-2", slippage_ticks: 2 },
        ],
        run_budget: 2,
        time_budget_seconds: 30,
      },
    })
  ).research_batch.state.definition.object;
  const tasksBeforeReviews = (await tasks()).map((item: { id: string }) => item.id).sort();
  await openView(page, "validation");
  const review = page.getByRole("region", { name: "有限批次稳健性审阅", exact: true });
  await review.getByRole("button", { name: "读取可审阅批次目录", exact: true }).click();
  await review
    .getByRole("combobox", { name: "完整批次来源", exact: true })
    .selectOption({ value: batch.id });
  await review.getByRole("button", { name: "读取完整批次预览", exact: true }).click();
  const evidenceTable = review.getByRole("table", {
    name: "全部声明成员与原账户金额",
    exact: true,
  });
  await expect(evidenceTable.getByRole("row")).toHaveCount(4);
  await review.getByText("原基准费用、保证金及合约单位（固定输入）", { exact: true }).click();
  await expect(review).toContainText(
    original.experiment.paper.contracts[0].dataset.contract.symbol,
  );
  await expect(
    review.getByRole("button", { name: "保存固定批次审阅", exact: true }),
  ).toBeDisabled();
  const assessmentList = async (id: string) =>
    (
      await rpc(page.request, "research.batch.assessment.list", {
        batch_id: id,
        after: "",
        limit: 30,
      })
    ).research_assessment.assessments;
  const declare = async (question: string, reason: string, selected = "") => {
    for (const [label, value] of [
      ["审阅问题", question],
      ["审阅范围", "整个固定批次，原完整账户口径"],
      ["人工判断理由", reason],
      ["选择或不选择理由", selected ? "仅临时引用已观察结果，不宣称赢家" : "明确不选候选"],
      ["未选成员及不利证据说明", "未选及未执行情景保留，不能补零或隐藏"],
      ["所见信息与用途", "已观察原账户和成本结果，不声称未使用的独立验证"],
    ])
      await review.getByLabel(label, { exact: true }).fill(value);
    await review
      .getByRole("combobox", { name: "明确人工判断", exact: true })
      .selectOption({ value: "insufficient" });
    await review
      .getByRole("combobox", { name: "被选原任务", exact: true })
      .selectOption({ value: selected });
    await review
      .getByLabel("已核对全批次、缺失与不利证据，明确保存人工判断；不授予交易权限", { exact: true })
      .check();
    await review.getByRole("button", { name: "保存固定批次审阅", exact: true }).click();
    await expect(
      review.getByRole("article", { name: "已保存固定批次审阅", exact: true }),
    ).toContainText(reason);
  };
  await declare("F10 全批次初次审阅", "成本情景已经观察，但仍缺独立验证，明确不选赢家");
  const firstReview = (await assessmentList(batch.id))[0];
  expect(firstReview.declaration.selected_task_id).toBe("");
  expect(firstReview.declaration.judgment).toBe("insufficient");
  expect(firstReview.snapshot.batch.definition.members).toHaveLength(2);
  expect(firstReview.snapshot.members).toHaveLength(2);
  expect(firstReview.snapshot.source.input_sha256).toBe(original.input_sha256);
  expect(firstReview.snapshot.source.result_sha256).toBe(original.task.result_digest);
  await review.getByRole("button", { name: "将此审阅引用为前次判断", exact: true }).click();
  await review.getByRole("button", { name: "读取完整批次预览", exact: true }).click();
  await review
    .getByLabel("追加修订理由", { exact: true })
    .fill("追加一个已观察成员的临时引用，不替换原判断");
  await review.getByLabel("已使用验证信息影响判断或选择", { exact: true }).check();
  await declare("F10 追加事后审阅", "新增明确引用，但范围仍是证据不足", variants[0].id);
  const history = await assessmentList(batch.id);
  expect(history).toHaveLength(2);
  expect(
    history.find((item: { object: { id: string } }) => item.object.id === firstReview.object.id),
  ).toEqual(firstReview);
  const appended = history.find(
    (item: { declaration: { previous_id: string } }) =>
      item.declaration.previous_id === firstReview.object.id,
  );
  expect(appended.declaration.selected_task_id).toBe(variants[0].id);
  expect(appended.declaration.validation_information_used).toBe(true);
  await review
    .getByRole("combobox", { name: "完整批次来源", exact: true })
    .selectOption({ value: feeBatch.id });
  await review.getByRole("button", { name: "读取完整批次预览", exact: true }).click();
  await expect(evidenceTable).toContainText("手续费倍率（合成情景）");
  await expect(evidenceTable).toContainText("1.5");
  await declare("F10 手续费批次审阅", "实际费用重跑且保留原成交核对，仍不宣称独立验证");
  const feeReview = (await assessmentList(feeBatch.id))[0];
  const feeExport = review.getByRole("region", { name: "固定批次审阅本地导出", exact: true });
  await feeExport
    .getByRole("combobox", { name: "批次审阅导出格式", exact: true })
    .selectOption({ value: "csv" });
  await feeExport
    .getByLabel("已核对固定审阅、原条件、人工文字及诊断范围，明确导出本地文件", { exact: true })
    .check();
  const feeDownloading = page.waitForEvent("download");
  await feeExport.getByRole("button", { name: "导出固定批次审阅", exact: true }).click();
  const feeDownload = await feeDownloading;
  const feeDocument = readFileSync((await feeDownload.path())!, "utf8");
  expect(feeDocument).toContain("member.0.fee_multiplier");
  expect(feeDocument).toContain("1.5");
  expect(feeDocument).toContain(feeReview.object.version);
  await review
    .getByRole("combobox", { name: "完整批次来源", exact: true })
    .selectOption({ value: idle.id });
  await review.getByRole("button", { name: "读取完整批次预览", exact: true }).click();
  await expect(evidenceTable.getByRole("row")).toHaveCount(4);
  for (const id of ["review-unstarted-1", "review-unstarted-2"]) {
    const row = evidenceTable.getByRole("row").filter({ hasText: id });
    await expect(row).toContainText("未接纳");
    await expect(row.getByText("未取得成功原结果", { exact: true })).toHaveCount(2);
  }
  await declare("F10 未执行批次审阅", "全部声明成员尚未接纳，不能把未取得结果当作零收益");
  const idleReview = (await assessmentList(idle.id))[0];
  expect(idleReview.snapshot.batch.phase).toBe("saved");
  expect(idleReview.snapshot.members).toHaveLength(0);
  expect((await tasks()).map((item: { id: string }) => item.id).sort()).toEqual(tasksBeforeReviews);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "f10-baseline" }))
      .task_result,
  ).toEqual(original);
  const output = review.getByRole("region", { name: "固定批次审阅本地导出", exact: true });
  const exportDirectory = mkdtempSync(join(tmpdir(), "asterion-assessment-browser-"));
  try {
    for (const format of ["html", "csv"]) {
      await output
        .getByRole("combobox", { name: "批次审阅导出格式", exact: true })
        .selectOption({ value: format });
      await output
        .getByLabel("已核对固定审阅、原条件、人工文字及诊断范围，明确导出本地文件", { exact: true })
        .check();
      const downloading = page.waitForEvent("download");
      await output.getByRole("button", { name: "导出固定批次审阅", exact: true }).click();
      const download = await downloading;
      expect(download.suggestedFilename()).toBe(`${idleReview.object.id}.assessment.${format}`);
      const content = readFileSync((await download.path())!, "utf8");
      for (const marker of [
        idleReview.object.version,
        "member.0.observed.account.net_profit",
        "member.1.task_id",
        "NOT_RECORDED",
        "NO_SELECTION",
        "NOT_ESTABLISHED",
        "source.contract.0.cost.0.open_fee",
      ])
        expect(content).toContain(marker);
      const path = join(exportDirectory, `fixed.${format}`);
      const parameters = { object: idleReview.object, format, path };
      const receipt = (await rpc(page.request, "research.batch.assessment.export", parameters))
        .research_export;
      expect(receipt.source_version).toBe(idleReview.object.version);
      expect(readFileSync(path, "utf8")).toBe(content);
      const repeated = await page.request.post("/__asterion/api", {
        data: { version: 1, method: "research.batch.assessment.export", params: parameters },
      });
      expect((await repeated.json()).error).toBeTruthy();
      expect(readFileSync(path, "utf8")).toBe(content);
      const wrong = await page.request.post("/__asterion/api", {
        data: {
          version: 1,
          method: "research.batch.assessment.export",
          params: {
            ...parameters,
            object: { ...idleReview.object, version: "0".repeat(64) },
            path: join(exportDirectory, `wrong.${format}`),
          },
        },
      });
      expect((await wrong.json()).error).toBeTruthy();
    }
  } finally {
    rmSync(exportDirectory, { recursive: true, force: true });
  }
  // Restart only this test's isolated Research and read exactly the same records.
  const stateBeforeRestart = await rpc(page.request, "runtime.snapshot");
  const researchService = stateBeforeRestart.research_service.service;
  await rpc(page.request, "node.action", { id: "local", service: researchService, action: "stop" });
  await rpc(page.request, "node.action", {
    id: "local",
    service: researchService,
    action: "start",
  });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).research_service?.online, {
      timeout: 10000,
    })
    .toBe(true);
  await expect
    .poll(
      async () =>
        (await assessmentList(idle.id)).map(
          (item: { object: { version: string } }) => item.object.version,
        ),
      { timeout: 10000 },
    )
    .toEqual([idleReview.object.version]);
  const reread = (
    await rpc(page.request, "research.batch.assessment.read", { id: idleReview.object.id })
  ).research_assessment.assessment;
  expect(reread).toEqual(idleReview);
  expect(
    (await rpc(page.request, "research.batch.assessment.read", { id: feeReview.object.id }))
      .research_assessment.assessment,
  ).toEqual(feeReview);
  expect(await assessmentList(batch.id)).toEqual(history);
  await page.reload();
  await openView(page, "validation");
  await review.getByRole("button", { name: "读取可审阅批次目录", exact: true }).click();
  await review
    .getByRole("combobox", { name: "完整批次来源", exact: true })
    .selectOption({ value: idle.id });
  await review.getByRole("button", { name: "读取该批次已保存审阅", exact: true }).click();
  await review
    .getByRole("combobox", { name: "已保存批次审阅", exact: true })
    .selectOption({ value: idleReview.object.id });
  await review.getByRole("button", { name: "读取固定批次审阅", exact: true }).click();
  await expect(
    review.getByRole("article", { name: "已保存固定批次审阅", exact: true }),
  ).toContainText(idleReview.object.version);
  await page.goto("/?screen=settings&category=resources");
  const catalogue = page.getByRole("region", { name: "业务资产目录", exact: true });
  await catalogue.getByLabel("资产所属域").selectOption("research");
  await catalogue.getByLabel("资产类别").selectOption("backtest_batch");
  await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
  const entry = catalogue.getByRole("row").filter({ hasText: batch.id });
  await expect(entry).toContainText(batch.version);
  await expect(entry).toContainText("已固定（只读）");
  await expect(entry.getByRole("button")).toHaveCount(0);
  // The resource catalogue is a settings screen, separate from the business workspace.
  await page.goto("/");
  // The same real daily source feeds the explicit fixed ledger evaluation.
  await openView(page, "risk");
  const risk = page.getByRole("region", { name: "风险分析与资金事实", exact: true });
  await risk
    .getByRole("combobox", { name: "评估来源", exact: true })
    .selectOption({ value: "f10-baseline" });
  await risk.getByRole("button", { name: "读取评估来源", exact: true }).click();
  const evaluation = risk.getByRole("region", { name: "固定绩效评价", exact: true });
  await evaluation
    .getByLabel("评价问题与用途", { exact: true })
    .fill("固定原日结账本核对，不授权交易或证明独立验证");
  await evaluation
    .getByLabel("保存原账本归因桥接（仅完整账户、单合约日线）", { exact: true })
    .check();
  await evaluation.getByRole("button", { name: "计算固定绩效评价", exact: true }).click();
  let ledgerId = "";
  await expect
    .poll(
      async () => {
        const task = (await tasks()).find(
          (item: { kind: string }) => item.kind === "performance_evaluation",
        );
        if (task) ledgerId = task.id;
        return task?.state;
      },
      { timeout: 30000 },
    )
    .toBe("succeeded");
  const ledgerReport = (
    await taskRpc(page.request, "research", "task.result.read", { id: ledgerId })
  ).task_result;
  const ledger = ledgerReport.result.ledger;
  expect(ledgerReport.experiment.request.include_ledger).toBe(true);
  expect(ledgerReport.experiment.request.scope).toBe("full_account");
  expect(ledger.reconciled).toBe(true);
  expect(ledger.residual).toBe("0");
  expect(ledger.fees).toBe(original.result.account.fees);
  expect(units(ledger.recorded_net)).toBe(
    units(original.result.account.equity) - units(original.experiment.paper.deposit),
  );
  expect(ledger.explained_net).toBe(ledger.recorded_net);
  expect(ledger.days).toHaveLength(original.result.settlements.length);
  for (let i = 0; i < ledger.days.length; ++i) {
    expect(ledger.days[i].source_index).toBe(i);
    expect(ledger.days[i].trading_day).toBe(original.result.settlements[i].trading_day);
    expect(ledger.days[i].recorded_balance).toBe(original.result.settlements[i].balance);
    expect(ledger.days[i].recorded_equity).toBe(original.result.settlements[i].equity);
  }
  expect(
    ledger.days.flatMap((day: { fills: { id: string }[] }) => day.fills.map(fill => fill.id)),
  ).toEqual(original.result.account.fills.map((fill: { id: string }) => fill.id));
  await evaluation
    .getByRole("combobox", { name: "已保存绩效评价", exact: true })
    .selectOption({ value: ledgerId });
  await evaluation.getByRole("button", { name: "读取固定绩效评价", exact: true }).click();
  const ledgerView = evaluation.getByRole("region", { name: "固定原账本归因桥接", exact: true });
  await expect(ledgerView.getByRole("status")).toContainText(
    "账本金额核对一致，不代表策略有效或已获交易许可",
  );
  const ledgerRows = ledgerView.getByRole("table", {
    name: "原日结毛变动费用与累计差额",
    exact: true,
  });
  await expect(ledgerRows.locator("tbody tr")).toHaveCount(ledger.days.length);
  const selectedDay = ledger.days.find((day: { fills: unknown[] }) => day.fills.length);
  expect(selectedDay).toBeTruthy();
  await ledgerRows
    .getByRole("row")
    .filter({ hasText: selectedDay.trading_day })
    .getByRole("button", { name: "查看原成交与核对项", exact: true })
    .click();
  await expect(
    ledgerView.getByRole("table", { name: "所选日结原成交", exact: true }),
  ).toContainText(selectedDay.fills[0].id);
  const ledgerOutput = evaluation.getByRole("region", { name: "固定评价本地导出", exact: true });
  await expect(ledgerOutput).toContainText("页面分页不缩小文件范围");
  const ledgerContext = (await rpc(page.request, "runtime.snapshot")).research_service;
  const ledgerExportDirectory = mkdtempSync(join(tmpdir(), "asterion-ledger-export-"));
  try {
    for (const format of ["html", "csv"]) {
      await ledgerOutput
        .getByRole("combobox", { name: "固定评价导出格式", exact: true })
        .selectOption({ value: format });
      await ledgerOutput
        .getByLabel("已核对固定报告、人工解释范围及全部限制，明确导出本地文件", { exact: true })
        .check();
      const downloading = page.waitForEvent("download");
      await ledgerOutput.getByRole("button", { name: "导出固定评价报告", exact: true }).click();
      const download = await downloading;
      expect(download.suggestedFilename()).toBe(`${ledgerId}.evaluation.${format}`);
      const content = readFileSync((await download.path())!, "utf8");
      for (const marker of [
        ledgerReport.task.result_digest,
        "ledger.explained_net",
        "ledger.residual",
        "ledger.closing_gross_quantity",
        selectedDay.fills[0].id,
      ])
        expect(content).toContain(marker);
      for (const day of ledger.days)
        expect(content).toContain(`ledger.day.${day.source_index}.cumulative_residual`);
      const fixedFields: [string, string][] = [
        ["scope", "full_account"],
        ["report.result_sha256", ledgerReport.task.result_digest],
        ["ledger.gross", ledger.gross],
        ["ledger.fees", ledger.fees],
        ["ledger.explained_net", ledger.explained_net],
        ["ledger.recorded_net", ledger.recorded_net],
        ["ledger.residual", ledger.residual],
      ];
      for (const day of ledger.days) {
        const prefix = `ledger.day.${day.source_index}`;
        for (const key of [
          "gross",
          "fees",
          "expected_balance",
          "recorded_balance",
          "recorded_equity",
          "cumulative_residual",
        ])
          fixedFields.push([`${prefix}.${key}`, day[key]]);
      }
      for (const [key, value] of fixedFields) {
        if (format === "csv") expect(content).toContain(`"${key}","${value}","RECORDED"`);
        else expect(content).toContain(`<th>${key}</th><td>${value}</td><td>RECORDED</td>`);
      }
      const path = join(ledgerExportDirectory, `ledger.${format}`);
      const parameters = {
        id: ledgerId,
        owner: {
          kind: "research",
          node_id: ledgerContext.node_id,
          service: ledgerContext.service,
          connection_id: ledgerContext.connection_id,
        },
        version: ledgerReport.task.result_digest,
        format,
        conclusion_id: "",
        conclusion_version: "",
        path,
      };
      const receipt = (await rpc(page.request, "evaluation.report.export", parameters))
        .research_export;
      expect(receipt.path).toBe(path);
      expect(receipt.source_version).toBe(ledgerReport.task.result_digest);
      expect(readFileSync(path, "utf8")).toBe(content);
      await expect(rpc(page.request, "evaluation.report.export", parameters)).rejects.toThrow();
      expect(readFileSync(path, "utf8")).toBe(content);
      const wrong = join(ledgerExportDirectory, `wrong.${format}`);
      await expect(
        rpc(page.request, "evaluation.report.export", {
          ...parameters,
          version: "0".repeat(64),
          path: wrong,
        }),
      ).rejects.toThrow();
      expect(existsSync(wrong)).toBe(false);
    }
  } finally {
    rmSync(ledgerExportDirectory, { recursive: true, force: true });
  }
  await rpc(page.request, "node.action", {
    id: "local",
    service: ledgerContext.service,
    action: "stop",
  });
  await rpc(page.request, "node.action", {
    id: "local",
    service: ledgerContext.service,
    action: "start",
  });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).research_service?.online)
    .toBe(true);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: ledgerId })).task_result,
  ).toEqual(ledgerReport);
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "f10-baseline" }))
      .task_result,
  ).toEqual(original);
  const current = await rpc(page.request, "runtime.snapshot");
  const selected = [current.data.service, current.research_service.service];
  // Only the isolated synthetic services created by this test wrapper are stopped.
  for (const service of selected)
    await rpc(page.request, "node.action", { id: "local", service, action: "stop" });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.filter((item: { id: string }) => selected.includes(item.id))
          .every(
            (item: { desired_running: boolean; pid: number }) => !item.desired_running && !item.pid,
          ),
      { timeout: 10000 },
    )
    .toBe(true);
  const temporary = mkdtempSync(join(tmpdir(), "asterion-batch-browser-"));
  try {
    const backup = join(temporary, "backup"),
      restored = join(temporary, "restored");
    await page.goto("/?screen=settings&category=backup");
    const maintenance = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    for (const service of selected)
      await maintenance.getByRole("checkbox", { name: new RegExp(service) }).check();
    await maintenance.getByLabel("新备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "预览备份", exact: true }).click();
    await maintenance
      .getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录", exact: true })
      .check();
    await maintenance.getByRole("button", { name: "保存备份", exact: true }).click();
    await expect(maintenance).toContainText("备份已保存并核验");
    const archivedManifest = (await rpc(page.request, "system.backup.inspect", { backup })).backup
      .manifest;
    const researchOwner = archivedManifest.owners.findIndex(
      (item: { domain: string; service: string }) =>
        item.domain === "research" && item.service === researchService,
    );
    expect(researchOwner).toBeGreaterThanOrEqual(0);
    // Inspect only immutable, isolated backup copies, never the active owner.
    const rows = (database: string): [string, string, string][] =>
      JSON.parse(
        execFileSync(
          "python3",
          [
            "-c",
            `
import json, sqlite3, sys
from pathlib import Path
path = Path(sys.argv[1]).resolve(strict=True)
connection = sqlite3.connect(path.as_uri() + "?mode=ro&immutable=1", uri=True)
print(json.dumps(connection.execute("SELECT id,batch_id,hex(body) FROM research_batch_assessments ORDER BY id").fetchall()))
connection.close()
`,
            database,
          ],
          { encoding: "utf8", timeout: 10000 },
        ),
      );
    const archivedRows = rows(join(backup, "owners", String(researchOwner), "tasks.sqlite"));
    const archivedLedger = readFileSync(
      join(backup, "owners", String(researchOwner), ledgerId, "results", "1.pb"),
    );
    expect(archivedLedger.length).toBeGreaterThan(0);

    expect(archivedRows.map(row => row[0])).toEqual(
      [firstReview.object.id, appended.object.id, feeReview.object.id, idleReview.object.id].sort(),
    );
    for (const fixed of [firstReview, appended, feeReview, idleReview]) {
      const body = archivedRows.find(row => row[0] === fixed.object.id)!;
      expect(Buffer.from(body[2], "hex").includes(Buffer.from(fixed.object.version, "utf8"))).toBe(
        true,
      );
    }
    await maintenance.getByLabel("已有备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "核验备份", exact: true }).click();
    await maintenance.getByLabel("新恢复目录", { exact: true }).fill(restored);
    await maintenance
      .getByRole("checkbox", { name: "确认恢复到独立新目录，不覆盖原数据或恢复授权", exact: true })
      .check();
    await maintenance.getByRole("button", { name: "恢复到新目录", exact: true }).click();
    await expect(maintenance).toContainText("已恢复到独立目录并核验，未启动服务");
    const manifest = (await rpc(page.request, "system.backup.inspect", { backup: restored })).backup
      .manifest;
    expect(manifest.owners).toHaveLength(2);
    expect(
      readFileSync(join(restored, "owners", String(researchOwner), ledgerId, "results", "1.pb")),
    ).toEqual(archivedLedger);

    expect(rows(join(restored, "owners", String(researchOwner), "tasks.sqlite"))).toEqual(
      archivedRows,
    );
    expect(
      manifest.dependencies.some(
        (item: { object: { kind: string; id: string; version: string } }) =>
          item.object.kind === "backtest_batch_assessment" &&
          item.object.id === firstReview.object.id &&
          item.object.version === firstReview.object.version,
      ),
    ).toBe(true);
    expect(manifest.dependencies.every((item: { included: boolean }) => item.included)).toBe(true);
    expect(
      manifest.dependencies.some(
        (item: { object: { kind: string; id: string; version: string } }) =>
          item.object.kind === "backtest_batch" &&
          item.object.id === batch.id &&
          item.object.version === batch.version,
      ),
    ).toBe(true);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
