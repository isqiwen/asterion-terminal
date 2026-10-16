import { createHash } from "node:crypto";
import { readFile, writeFile, unlink } from "node:fs/promises";
import { resolve } from "node:path";
import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";

test("research chain integration @joint: fixed evidence through revision recovery and restart", async ({
  page,
}, info) => {
  test.setTimeout(240000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const control = process.env.ASTERION_TABLE_FIXTURE_CONTROL!;
  expect(control).toBeTruthy();
  const records: Record<string, unknown> = {};
  const result = async (id: string) =>
    (await rpc(page.request, "task.result.read", { id })).task_result;
  const state = async () => rpc(page.request, "runtime.snapshot");
  const tasks = async () => (await state()).task_service.tasks;
  async function wait(id: string, expected = "succeeded") {
    await expect
      .poll(async () => (await tasks()).find((t: { id: string }) => t.id === id)?.state, {
        timeout: 30000,
      })
      .toBe(expected);
  }
  async function screenshot(name: string) {
    const path = info.outputPath(`${name}.png`);
    await page.screenshot({ path, fullPage: true });
    await info.attach(name, { path, contentType: "image/png" });
  }
  async function openResearch(kind: "factor" | "backtest", id: string) {
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page
      .getByRole("navigation", { name: "研究页面", exact: true })
      .getByRole("button", { name: kind === "factor" ? "因子" : "回测", exact: true })
      .click();
    const workspace = page.getByRole("region", { name: "期货研究", exact: true });
    const back = workspace.getByRole("button", { name: "返回回测记录", exact: true });
    if (kind === "backtest" && (await back.isVisible())) await back.click();
    await workspace
      .getByRole("region", { name: "研究任务", exact: true })
      .getByRole("row")
      .filter({ has: page.getByText(id, { exact: true }) })
      .getByRole("button", { name: "查看结果", exact: true })
      .click();
    return workspace;
  }
  try {
    let daily: {
      snapshot: string;
      daily_source: string;
      calendar_source: string;
      contracts_source: string;
    };
    const days = Array.from({ length: 80 }, (_, i) =>
      new Date(Date.UTC(2024, 0, 2 + i)).toISOString().slice(0, 10),
    )
      .filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()))
      .slice(0, 52);
    const prices = days.map(
      (_, i) => [100, 102, 106, 110, 104, 98, 94, 100, 107, 112, 103, 95, 90][i % 13],
    );
    await test.step("一份固定日线证据与三标的资格筛选", async () => {
      const build = process.env.ASTERION_CPP_BUILD ?? resolve(__dirname, "../../../../build/Debug");
      const path = resolve(
        build,
        "tests/support/native-plugin/good/plugin_fixture_good" +
          (process.platform === "darwin" ? ".dylib" : ".so"),
      );
      const candidate = (await rpc(page.request, "native.plugins.preview", { path }))
        .plugin_candidate;
      await rpc(page.request, "native.plugins.install", { path, sha256: candidate.sha256 });
      const stopped = await rpc(page.request, "node.action", {
        id: "local",
        service: "historical-data",
        action: "stop",
      });
      const service = stopped.nodes
        .find((n: { id: string }) => n.id === "local")
        .health.services.find((s: { id: string }) => s.id === "historical-data");
      const plugins = stopped.native_plugins.items
        .filter(
          (item: { managed?: boolean; state: string; capabilities: { id: string }[] }) =>
            !item.managed &&
            item.state === "available" &&
            item.capabilities.some(c =>
              [
                "asterion.history.v3",
                "asterion.history.tables.v1",
                "asterion.risk.pre-trade.v1",
              ].includes(c.id),
            ),
        )
        .map((item: { sha256: string }) => item.sha256);
      await rpc(page.request, "node.plugins.configure", {
        id: "local",
        service: "historical-data",
        revision: service.revision,
        plugins: [...plugins, candidate.sha256],
      });
      await rpc(page.request, "node.data_tasks.local.open");
      for (const [i, product] of ["rb", "cu", "al"].entries()) {
        const seeded = await seedHistory(
          page.request,
          prices.map(p => p + (product === "al" ? 10 : 0)),
          `joint-${product}`,
          {
            provider: "test.independent.c",
            product,
            month: "2024-03",
            tablesOnly: true,
            dailyPrices: true,
            dailyDays: days,
            keep: i > 0,
            openInterest: product === "cu" ? 50 : 200,
          },
        );
        daily = seeded.daily;
      }
      expect(daily!.daily_source).toBe("fixture.daily.v1");
      await rpc(page.request, "factor.submit", {
        id: "joint-factor",
        series: {
          kind: "tables",
          snapshot: daily!.snapshot,
          daily_source: daily!.daily_source,
          calendar_source: daily!.calendar_source,
          end_day: days.at(-1)!,
          ranges: [
            {
              begin_day: days[0],
              instruments: ["SHFE/rb/2024-03", "SHFE/cu/2024-03", "SHFE/al/2024-03"],
            },
          ],
        },
        factor: "momentum",
        lookbacks: [2],
        horizon: 1,
        evaluation: { mode: "full_sample" },
        liquidity: { minimum_history: 3, minimum_volume: "0", minimum_open_interest: "100" },
      });
      await wait("joint-factor");
      const factor = await result("joint-factor");
      const cu = factor.experiment.series.findIndex(
        (s: { data: { instrument: string } }) => s.data.instrument === "SHFE/cu/2024-03",
      );
      expect(cu).toBeGreaterThanOrEqual(0);
      expect(
        factor.result.memberships.some(
          (m: { series_index: number; reason: string }) =>
            m.series_index === cu && m.reason === "MEMBERSHIP_BELOW_OPEN_INTEREST",
        ),
      ).toBe(true);
      records.initial_snapshot = daily!.snapshot;
      records.factor_original = factor;
    });
    await page.goto("/");
    let factorVariant = "";
    await test.step("固定数据窗口派生、比较与导出", async () => {
      const workspace = await openResearch("factor", "joint-factor");
      const reuse = workspace.getByRole("region", { name: "按此日线实验创建", exact: true });
      await reuse.getByRole("button", { name: "修改回看窗口并派生", exact: true }).click();
      await reuse.getByLabel("派生回看日线数", { exact: true }).fill("5");
      await reuse.getByLabel("沿用原固定快照", { exact: true }).check();
      await reuse.getByRole("button", { name: "核对参数派生", exact: true }).click();
      await reuse.getByRole("button", { name: "确认创建派生因子任务", exact: true }).click();
      factorVariant = (await reuse.getByRole("status").locator("code").textContent())!;
      await wait(factorVariant);
      const variant = await result(factorVariant),
        original = await result("joint-factor");
      expect(variant.experiment.lookbacks).toEqual([5]);
      expect(variant.experiment.series).toEqual(original.experiment.series);
      expect(variant.experiment.liquidity).toEqual(original.experiment.liquidity);
      expect(original).toEqual(records.factor_original);
      records.factor_variant = variant;
      const comparison = workspace.getByRole("region", { name: "日线因子比较", exact: true });
      for (const id of ["joint-factor", factorVariant])
        await comparison.getByRole("checkbox", { name: new RegExp(id) }).check();
      await comparison.getByRole("button", { name: "读取并比较", exact: true }).click();
      await expect(
        comparison.getByRole("table", { name: "因子实验对照", exact: true }),
      ).toContainText("同口径对照");
      await expect(
        comparison.getByRole("table", { name: "因子实验对照", exact: true }),
      ).toContainText("实际评价观测不同");
      const downloadEvent = page.waitForEvent("download");
      await comparison.getByRole("button", { name: "导出当前比较证据", exact: true }).click();
      const download = await downloadEvent,
        path = info.outputPath("factor-comparison.json");
      await download.saveAs(path);
      const bytes = await readFile(path),
        exported = JSON.parse(bytes.toString());
      expect(exported.experiments.map((e: { experiment: unknown }) => e.experiment)).toEqual([
        original.experiment,
        variant.experiment,
      ]);
      records.factor_export_sha256 = createHash("sha256").update(bytes).digest("hex");
      await info.attach("factor comparison evidence", { path, contentType: "application/json" });
      await screenshot("01-fixed-factor-comparison");
    });
    await test.step("同一证据回测、费用/月贡献/日期持有期", async () => {
      await rpc(page.request, "data.dataset.daily", {
        contract_id: "SHFE/rb/2024-03",
        daily: daily!,
        begin_day: days[0],
        end_day: days.at(-1)!,
      });
      await rpc(page.request, "backtest.submit", {
        id: "joint-backtest",
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
            symbol: "rb2403",
            slippage_ticks: 0,
            cost_schedule: [
              {
                effective_from: "1970-01-01",
                source: "joint synthetic assumption",
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
      await wait("joint-backtest");
      const base = await result("joint-backtest");
      expect(base.result.account.fills.length).toBeGreaterThan(0);
      expect(Number(base.result.account.fees)).toBeGreaterThan(0);
      expect(base.performance.by_month.length).toBe(3);
      expect(base.trades.holding_summary.known).toBe(base.trades.count);
      expect(base.trades.open.length).toBeGreaterThan(0);
      expect(base.reconciliation.state).toBe("available");
      expect(base.reconciliation.days).toHaveLength(days.length);
      expect(
        base.reconciliation.final_checks.every((check: { matches: boolean }) => check.matches),
      ).toBe(true);
      for (const day of base.reconciliation.days)
        expect(day.checks.every((check: { matches: boolean }) => check.matches)).toBe(true);
      const fillDay = base.reconciliation.days.find(
        (day: { fills: unknown[] }) => day.fills.length > 0,
      )!;
      expect(base.reconciliation.model.cost_source).toBe("joint synthetic assumption");
      records.backtest_original = base;
      const workspace = await openResearch("backtest", "joint-backtest");
      const stats = workspace.getByRole("region", { name: "按笔统计", exact: true });
      await stats.getByText("持有期（交易日期口径）", { exact: true }).click();
      await expect(stats.getByRole("table", { name: "已平仓持有期", exact: true })).toBeVisible();
      await workspace.getByText("按月收益贡献", { exact: true }).click();
      await expect(
        workspace.getByRole("table", { name: "按月收益贡献", exact: true }),
      ).toBeVisible();
      await workspace.getByText("成交与资金核对", { exact: true }).click();
      const auditTable = workspace.getByRole("table", { name: "逐日成交资金核对", exact: true });
      await expect(auditTable).toBeVisible();
      const fillRow = auditTable.getByRole("row").filter({ hasText: fillDay.day });
      await fillRow.getByRole("button").click();
      const receipt = workspace.getByRole("region", { name: "当日核对证据", exact: true });
      await expect(receipt.getByText(fillDay.fills[0].id, { exact: true })).toBeVisible();
      await receipt.scrollIntoViewIfNeeded();
      const receiptPath = info.outputPath("02b-selected-fill-evidence.png");
      await receipt.screenshot({ path: receiptPath });
      await info.attach("selected fill reconciliation", {
        path: receiptPath,
        contentType: "image/png",
      });
      await expect(workspace.getByText(/历史保证金、乘数及规格适用性未在此证实/)).toBeVisible();
      await screenshot("02a-fixed-account-reconciliation");
      const downloadEvent = page.waitForEvent("download");
      await workspace.getByRole("button", { name: "导出研究证据", exact: true }).click();
      const download = await downloadEvent,
        path = info.outputPath("backtest-evidence.json");
      await download.saveAs(path);
      const bytes = await readFile(path),
        doc = JSON.parse(bytes.toString());
      expect(doc.result).toEqual(base.result);
      expect(doc.trades).toEqual(base.trades);
      expect(doc.performance).toEqual(base.performance);
      expect(doc.reconciliation).toEqual(base.reconciliation);
      records.backtest_export_sha256 = createHash("sha256").update(bytes).digest("hex");
      await info.attach("backtest attribution evidence", { path, contentType: "application/json" });
      await screenshot("02-account-attribution");
    });
    await test.step("有限滑点变体保持同一完整输入", async () => {
      const workspace = await openResearch("backtest", "joint-backtest");
      const panel = workspace.getByRole("region", { name: "固定数据滑点敏感性", exact: true });
      await panel.getByLabel("滑点变体（1–2个整数，以逗号分隔）", { exact: true }).fill("1,2");
      await panel.getByRole("button", { name: "核对滑点变体", exact: true }).click();
      await panel.getByRole("button", { name: "确认创建滑点变体", exact: true }).click();
      await expect
        .poll(
          async () =>
            (await tasks()).filter(
              (t: { kind: string; id: string; state: string }) =>
                t.kind === "backtest" && t.id !== "joint-backtest" && t.state === "succeeded",
            ).length,
        )
        .toBe(2);
      const variants = (await tasks()).filter(
        (t: { kind: string; id: string }) => t.kind === "backtest" && t.id !== "joint-backtest",
      );
      const baseline = await result("joint-backtest");
      for (const task of variants) {
        const v = await result(task.id),
          n = structuredClone(v.experiment);
        n.paper.contracts[0].slippage_ticks = 0;
        expect(n).toEqual(baseline.experiment);
        expect(v.result.account.equity).not.toBe(baseline.result.account.equity);
      }
      await panel.getByRole("button", { name: "读取滑点敏感性结果", exact: true }).click();
      await expect(
        panel.getByRole("table", { name: "滑点敏感性结果", exact: true }).locator("tbody tr"),
      ).toHaveCount(3);
      records.slippage_ids = variants.map((t: { id: string }) => t.id);
      await screenshot("03-slippage-sensitivity");
    });
    await test.step("单分区修订、失败重试、空响应与未变", async () => {
      await rpc(page.request, "data.credentials.save", {
        provider: "test.independent.c",
        credential: "joint-fixture-only",
        remember: false,
        requests_per_minute: 60,
      });
      await rpc(page.request, "data.download.budget.configure", {
        source: "fixture.minutes",
        token: "",
        requests_per_minute: 60,
      });
      await page
        .locator(".workspace-tabs")
        .getByRole("button", { name: "数据", exact: true })
        .click();
      await page.getByRole("button", { name: "全市场数据", exact: true }).click();
      const tables = page.getByRole("region", { name: "全市场数据", exact: true });
      await tables
        .getByRole("table", { name: "全市场数据覆盖", exact: true })
        .getByRole("row")
        .filter({ hasText: "日线" })
        .getByRole("button", { name: "查看数据", exact: true })
        .click();
      const partition = tables.getByRole("region", { name: "分区数据", exact: true });
      await partition.getByLabel("交易日", { exact: true }).fill("2024-03-01");
      await partition.getByRole("button", { name: "查看", exact: true }).click();
      const panel = partition.getByRole("region", { name: "重新获取此日分区", exact: true });
      const head = async () =>
        (await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot;
      async function submit(mode: string) {
        await writeFile(control, mode);
        const before = await head();
        await panel.getByRole("button", { name: "预览重新获取", exact: true }).click();
        expect(await head()).toBe(before);
        await panel.getByRole("button", { name: "确认重新获取此分区", exact: true }).click();
        const id = (await panel
          .getByRole("status")
          .filter({ hasText: "重新获取任务已提交" })
          .textContent())!
          .split(":")
          .at(-1)!
          .trim();
        return id;
      }
      let id = await submit("base");
      await wait(id);
      expect(await head()).not.toBe(daily!.snapshot);
      await panel.getByRole("button", { name: "读取当前分区", exact: true }).click();
      await expect(partition.getByRole("table", { name: "分区数据", exact: true })).toContainText(
        "72000.5",
      );
      const beforeFailure = await head();
      id = await submit("fail-once");
      await wait(id, "failed");
      expect(await head()).toBe(beforeFailure);
      await expect(panel).toContainText("来源不可用或资源受限");
      await screenshot("04-refresh-recovery");
      await panel.getByRole("button", { name: "重试此任务", exact: true }).click();
      await wait(id);
      expect((await tasks()).find((t: { id: string }) => t.id === id).attempt).toBe(2);
      expect(await head()).not.toBe(beforeFailure);
      await panel.getByRole("button", { name: "读取当前分区", exact: true }).click();
      await expect(partition.getByRole("table", { name: "分区数据", exact: true })).toContainText(
        "72001.5",
      );
      const beforeEmpty = await head();
      id = await submit("empty");
      await wait(id);
      await expect(panel).toContainText("原因与历史覆盖均未知");
      expect(await head()).toBe(beforeEmpty);
      await panel.getByRole("button", { name: "读取当前分区", exact: true }).click();
      id = await submit("corrected");
      await wait(id);
      await expect(panel).toContainText("内容未变，未新增修订");
      expect(await head()).toBe(beforeEmpty);
      records.revised_snapshot = await head();
      expect(await result("joint-factor")).toEqual(records.factor_original);
      expect(await result("joint-backtest")).toEqual(records.backtest_original);
    });
    await test.step("显式新快照复用与原固定实验并存", async () => {
      const original = await result("joint-factor");
      await rpc(page.request, "factor.reuse", {
        id: "joint-factor-revised",
        source_id: original.id,
        source_result_digest: original.task.result_digest,
        experiment: original.experiment,
        mode: "snapshot",
        snapshot: records.revised_snapshot,
      });
      await wait("joint-factor-revised");
      const revised = await result("joint-factor-revised");
      expect(revised.experiment.dataset_revision).not.toBe(original.experiment.dataset_revision);
      expect(revised.experiment.liquidity).toEqual(original.experiment.liquidity);
      records.factor_revised = revised;
      expect(await result("joint-factor")).toEqual(records.factor_original);
    });
    await test.step("服务重启后重读全部结果、档案与修订", async () => {
      const before = await state();
      const snapshots = await Promise.all((await tasks()).map((t: { id: string }) => result(t.id)));
      const services = [before.data.service, before.task_service.service];
      const pids = before.nodes
        .find((n: { id: string }) => n.id === "local")
        .health.services.filter((s: { id: string }) => services.includes(s.id))
        .map((s: { id: string; pid: number }) => ({ id: s.id, pid: s.pid }));
      for (const service of [...services].reverse())
        await rpc(page.request, "node.action", { id: "local", service, action: "stop" });
      for (const service of services)
        await rpc(page.request, "node.action", { id: "local", service, action: "start" });
      await rpc(page.request, "node.data_tasks.local.open");
      await expect
        .poll(
          async () => {
            const v = await state();
            return !!v.data?.online && !!v.task_service?.online;
          },
          { timeout: 30000 },
        )
        .toBe(true);
      for (const saved of snapshots) expect(await result(saved.id)).toEqual(saved);
      expect((await rpc(page.request, "data.tables.coverage")).table_coverage.snapshot).toBe(
        records.revised_snapshot,
      );
      const after = await state();
      for (const old of pids)
        expect(
          after.nodes
            .find((n: { id: string }) => n.id === "local")
            .health.services.find((s: { id: string }) => s.id === old.id).pid,
        ).not.toBe(old.pid);
      records.restarted_services = pids;
      records.task_ids = snapshots.map(v => v.id);
      records.restarted_result_count = snapshots.length;
      // The owner reset can move a stale result view back to records after
      // its back button was observed. Reload after verifying recovery, then
      // wait for the actual records view before selecting the result.
      await page.reload();
      await page
        .locator(".workspace-tabs")
        .getByRole("button", { name: "研究", exact: true })
        .click();
      await page
        .getByRole("navigation", { name: "研究页面", exact: true })
        .getByRole("button", { name: "回测", exact: true })
        .click();
      await expect(
        page
          .getByRole("region", { name: "期货研究", exact: true })
          .getByRole("heading", { name: "回测记录", exact: true }),
      ).toBeVisible();
      const workspace = await openResearch("backtest", "joint-backtest");
      await expect(workspace.getByRole("region", { name: "按笔统计", exact: true })).toBeVisible();
      await screenshot("05-restarted-original-result");
    });
    const path = info.outputPath("joint-evidence.json");
    await writeFile(path, JSON.stringify(records, null, 2));
    await info.attach("joint evidence and original results", {
      path,
      contentType: "application/json",
    });
  } finally {
    await unlink(control).catch(() => {});
  }
});
