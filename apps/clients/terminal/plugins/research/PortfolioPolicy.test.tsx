import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { DatasetSelection, PortfolioTargetDecision } from "../../src/bridge/client";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { backtestEvidence } from "./testing/backtestEvidence";
import { portfolioPolicy, portfolioPolicyDefaults, portfolioEligible } from "./PortfolioPolicy";
import type { ReferenceStrategyPackage, ReferenceStrategyDelivery } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { PortfolioTargetsEvidence } from "./PortfolioTargetsEvidence";
import { Backtest } from "./Backtest";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
function dataset(symbol: string): DatasetSelection {
  const data = backtestEvidence("template").experiment.paper.contracts[0].dataset;
  return {
    source_dataset_ids: [],
    daily: data.daily,
    begin_day: "2026-09-21",
    end_day: "2026-09-28",
    contract: { ...data.contract, symbol, product: symbol.slice(0, 2) },
    price_increment_changes: [],
    venue: "SHFE",
    symbol,
    revision: "b".repeat(64),
    source: "fixture.daily",
    interval_minutes: 0,
    count: 6,
    days: 6,
    first_day: "2026-09-21",
    last_day: "2026-09-28",
    first_timestamp_ns: "1790000000000000000",
    last_timestamp_ns: "1790600000000000000",
    last_close: "1000",
    uncovered_days: [],
  };
}

test("workspace policy has no implicit weights or budgets and preserves explicit zero and exact budget limits", () => {
  const data = [dataset("rb2610"), dataset("hc2610")];
  expect(portfolioEligible(data, false)).toBe(true);
  expect(portfolioPolicy({ ...portfolioPolicyDefaults, enabled: true }, data, true)).toBeNull();
  const draft = {
    enabled: true,
    weights: { "SHFE/rb2610": "0.4", "SHFE/hc2610": "-0.2" },
    gross: "0.6",
    net: "0.2",
    rebalance: "2",
  };
  const fixed = portfolioPolicy(draft, data, true)!;
  expect(fixed.rule.kind).toBe("fixed_weights");
  expect("quantity" in fixed).toBe(false);
  expect(portfolioPolicy({ ...draft, gross: "0.59999999" }, data, true)).toBeNull();
  expect(portfolioPolicy({ ...draft, weights: { "SHFE/rb2610": "0.4" } }, data, true)).toBeNull();
  expect(
    portfolioPolicy(
      { ...draft, weights: { "SHFE/rb2610": "0", "SHFE/hc2610": "0" }, gross: "0", net: "0" },
      data,
      true,
    )?.rule.members.map(m => m.weight),
  ).toEqual(["0", "0"]);
  expect(portfolioEligible([{ ...data[0], interval_minutes: 1 }, data[1]], false)).toBe(false);
  expect(portfolioEligible(data, true)).toBe(false);
});

test("actual backtest confirmation submits only the explicit portfolio policy with original account and costs", async () => {
  const snapshot = readySnapshot();
  snapshot.datasets = [dataset("rb2610"), dataset("hc2610")];
  const store = createWorkspaceDrafts();
  const ns = "portfolio-form";
  const put = (slot: string, value: unknown) => store.write(JSON.stringify([ns, slot]), value);
  put(`backtest-view:${snapshot.research_service!.connection_id}`, "configure");
  put("backtest-step", 1);
  put("backtest-account", {
    deposit: "100000",
    max_order_quantity: "100",
    max_gross_quantity: "100",
    max_working_orders: "100",
  });
  const original = backtestEvidence("costs").experiment.paper.contracts[0];
  put(
    "contract-costs",
    Object.fromEntries(
      snapshot.datasets.map(d => [
        `${d.venue}.${d.symbol}`,
        {
          ...original.cost_schedule[0].values,
          cost_schedule: original.cost_schedule,
          slippage_ticks: "0",
        },
      ]),
    ),
  );
  const trade = vi.fn(async (_method: string, _params?: Record<string, unknown>) => {});
  const query = vi.fn(async () => snapshot);
  render(
    <WorkspaceDraftScope store={store} namespace={ns}>
      <Backtest
        snapshot={snapshot}
        busy={false}
        error=""
        openSettings={vi.fn()}
        trade={trade}
        query={query}
        navigate={vi.fn()}
        workspacePage="backtest"
      />
    </WorkspaceDraftScope>,
  );
  const option = screen.getByLabelText(
    "使用固定权重组合政策（仅完整普通日线）",
  ) as HTMLInputElement;
  expect(option.checked).toBe(false);
  fireEvent.click(option);
  expect(screen.queryByLabelText("快均线")).toBeNull();
  const next = screen.getByRole("button", { name: "下一步" });
  expect((next as HTMLButtonElement).disabled).toBe(true);
  for (const [label, value] of [
    ["原合约名义权重 SHFE/rb2610", "0.4"],
    ["原合约名义权重 SHFE/hc2610", "-0.2"],
    ["组合总绝对名义权重预算", "0.6"],
    ["组合净名义权重预算", "0.2"],
    ["每隔共同完整bar调仓（1–10000，必填）", "2"],
  ])
    fireEvent.change(screen.getByLabelText(label), { target: { value } });
  await waitFor(() => expect((next as HTMLButtonElement).disabled).toBe(false));
  expect(trade).not.toHaveBeenCalled();
  fireEvent.click(next);
  const summary = screen.getByText("固定组合名义政策").closest("dl")!;
  expect(summary.textContent).toContain("SHFE/rb2610：0.4");
  expect(summary.textContent).toContain("SHFE/hc2610：-0.2");
  expect(trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "开始回测" }));
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(1));
  expect(trade.mock.calls[0][0]).toBe("backtest.submit");
  expect(trade.mock.calls[0][1]).toMatchObject({
    deposit: "100000",
    max_gross_quantity: "100",
    strategies: [
      {
        sides: "both",
        rule: {
          kind: "fixed_weights",
          rebalance: 2,
          gross_weight_limit: "0.6",
          net_weight_limit: "0.2",
          members: [
            { venue: "SHFE", symbol: "rb2610", weight: "0.4" },
            { venue: "SHFE", symbol: "hc2610", weight: "-0.2" },
          ],
        },
      },
    ],
    contracts: [
      { venue: "SHFE", symbol: "rb2610", slippage_ticks: 0 },
      { venue: "SHFE", symbol: "hc2610", slippage_ticks: 0 },
    ],
  });
  store.dispose();
});

test("original portfolio targets and explicit fill associations remain read-only through pagination", () => {
  const evidence = backtestEvidence("portfolio");
  evidence.experiment.strategies = [
    {
      sides: "both",
      rule: {
        kind: "fixed_weights",
        rebalance: 2,
        gross_weight_limit: "0.8",
        net_weight_limit: "0.3",
        members: [
          { venue: "SHFE", symbol: "rb2610", weight: "0.4" },
          { venue: "SHFE", symbol: "hc2610", weight: "-0.2" },
        ],
      },
    },
  ];
  const input = evidence.experiment.paper.contracts[0];
  evidence.result.account.contracts = [
    {
      contract: input.dataset.contract,
      costs: input.cost_schedule[0].values,
      cost_schedule: input.cost_schedule,
      mark: "1000",
    },
  ];
  evidence.result.portfolio_targets = Array.from(
    { length: 51 },
    (_, i): PortfolioTargetDecision => ({
      boundary_ns: String(BigInt(Date.UTC(2026, 0, 2 + i * 2)) * 1000000n),
      trading_day: new Date(Date.UTC(2026, 0, 2 + i * 2)).toISOString().slice(0, 10),
      has_next_bar: i < 50,
      equity: "100000.0000001",
      gross_weight: "0.6",
      net_weight: "0.2",
      gross_limit: "0.8",
      net_limit: "0.3",
      requested_gross_notional: "60000.00000006",
      requested_net_notional: "20000.00000002",
      allocated_gross_notional: "60000",
      allocated_net_notional: "20000",
      unallocated_nominal_budget: "20000.00000002",
      members: [
        {
          venue: "SHFE",
          symbol: "rb2610",
          source_index: i * 4 + 2,
          intent_id: `strategy.${i * 4 + 2}`,
          weight: "0.4",
          price: "1000",
          multiplier: "10",
          requested_notional: "40000.00000004",
          quantity: "4",
          allocated_notional: "40000",
          nominal_remainder: "0.00000004",
          observation: { positions: [], working_orders: [] },
        },
        {
          venue: "SHFE",
          symbol: "hc2610",
          source_index: i * 4 + 3,
          intent_id: `strategy.${i * 4 + 3}`,
          weight: "-0.2",
          price: "1000",
          multiplier: "10",
          requested_notional: "-20000.00000002",
          quantity: "-2",
          allocated_notional: "-20000",
          nominal_remainder: "-0.00000002",
          observation: { positions: [], working_orders: [] },
        },
      ],
    }),
  );
  evidence.result.account.fills = [
    {
      id: "actual-A",
      order_id: "original-order",
      venue: "SHFE",
      symbol: "rb2610",
      quantity: "4",
      price: "1000",
      fee: "8",
    },
  ];
  evidence.result.account.orders = [
    {
      id: "original-order",
      venue: "SHFE",
      symbol: "rb2610",
      side: "buy",
      offset: "open",
      quantity: "4",
      limit_price: "1000",
      filled: "1",
      state: "cancelled",
    },
  ];
  evidence.result.account.fills[0].quantity = "1";
  evidence.result.account.fills[0].fee = "2";
  evidence.result.portfolio_orders = [
    {
      order_id: "original-order",
      intent_id: "strategy.2",
      decision_source_index: 2,
      source_index: 4,
      boundary_ns: String(BigInt(Date.UTC(2026, 0, 3)) * 1000000n),
    },
  ];
  evidence.result.portfolio_fills = [
    {
      fill_id: "actual-A",
      intent_id: "strategy.2",
      decision_source_index: 2,
      source_index: 4,
      boundary_ns: String(BigInt(Date.UTC(2026, 0, 3)) * 1000000n),
    },
  ];
  evidence.result.portfolio_targets![50].members[0].observation.positions = [
    {
      venue: "SHFE",
      symbol: "rb2610",
      side: "buy",
      bucket: "yesterday",
      quantity: "1",
      basis: "1000.00000001",
    },
  ];
  const before = JSON.stringify(evidence);
  render(<PortfolioTargetsEvidence evidence={evidence} />);
  const detail = screen.getByRole("region", { name: "所选原组合目标与成交" });
  expect(detail.textContent).toContain("40000.00000004");
  expect(detail.textContent).toContain("0.00000004");
  expect(within(detail).getByRole("table", { name: "原组合意图的实际成交" }).textContent).toContain(
    "actual-A",
  );
  const orders = within(detail).getByRole("table", { name: "原组合意图的实际接纳委托" });
  const cells = within(within(orders).getAllByRole("row")[1]).getAllByRole("cell");
  expect(cells[0].textContent).toBe("strategy.2");
  expect(cells[2].textContent).toBe("4");
  expect(cells[4].textContent).toBe("original-order");
  expect(cells[7].textContent).toBe("4");
  expect(cells[8].textContent).toBe("1");
  expect(cells[9].textContent).toBe("已撤余量");
  expect(detail.textContent).toContain("委托状态取自原回测终点");
  fireEvent.click(screen.getByRole("button", { name: "下一页组合边界" }));
  const rows = within(screen.getByRole("table", { name: "全部原组合调仓边界" })).getAllByRole(
    "row",
  );
  expect(rows).toHaveLength(2);
  expect(rows[1].textContent).toContain("末边界，无后续bar成交");
  fireEvent.click(screen.getByRole("button", { name: "查看原组合目标与成交" }));
  expect(screen.getByRole("region", { name: "所选原组合目标与成交" }).textContent).toContain(
    "本轮未记录实际成交",
  );
  const holdings = screen.getByRole("table", { name: "原调仓边界实际持仓批次" });
  expect(holdings.textContent).toContain("1000.00000001");
  expect(holdings.textContent).toContain("昨仓");
  expect(holdings.textContent).toContain("多头");
  expect(JSON.stringify(evidence)).toBe(before);
});

test("portfolio delivery locks every policy member while missing data blocks and new account conditions remain explicit", async () => {
  const snapshot = readySnapshot();
  const choices = [dataset("rb2610"), dataset("hc2610")];
  snapshot.datasets = choices.slice(0, 1);
  if (!snapshot.research_service) throw new Error("missing research fixture");
  const resource = {
    node: "fixture-node",
    service: snapshot.research_service.service,
    domain: "research" as const,
    kind: "strategy_package",
    id: "portfolio-candidate",
    version: "c".repeat(64),
  };
  const candidate: ReferenceStrategyPackage = {
    object: resource,
    name: "Fixed portfolio delivery",
    purpose: "Explicit independent conditions",
    limitations: "Synthetic retrospective reference only",
    definition: {
      interval_minutes: 0,
      program_artifact: "a".repeat(64),
      risk_artifact: "b".repeat(64),
      portfolio: {
        contracts: choices.map(d => d.contract),
        strategy: {
          sides: "both",
          rule: {
            kind: "fixed_weights",
            rebalance: 2,
            gross_weight_limit: "0.8",
            net_weight_limit: "0.3",
            members: [
              { venue: "SHFE", symbol: "rb2610", weight: "0.4" },
              { venue: "SHFE", symbol: "hc2610", weight: "-0.2" },
            ],
          },
        },
      },
    },
    evidence_relation: "reference_only",
    reference_result: {
      ...resource,
      kind: "backtest_result",
      id: "original-source",
      version: "d".repeat(64),
    },
    source_input_sha256: "e".repeat(64),
    source_attempt: 1,
    recorded_at_ms: "1",
    retirement_reason: "",
  };
  const delivery: ReferenceStrategyDelivery = {
    object: {
      ...resource,
      kind: "strategy_delivery",
      id: "explicit-delivery",
      version: "f".repeat(64),
    },
    package: candidate.object,
    reference_result: candidate.reference_result,
    source_input_sha256: candidate.source_input_sha256,
    target_task_id: "explicit-portfolio-target",
    recorded_at_ms: "2",
  };
  const store = createWorkspaceDrafts();
  const ns = "portfolio-explicit";
  const put = (slot: string, value: unknown) => store.write(JSON.stringify([ns, slot]), value);
  put("backtest-account", {
    deposit: "100000",
    max_order_quantity: "100",
    max_gross_quantity: "100",
    max_working_orders: "100",
  });
  const cost = backtestEvidence("costs").experiment.paper.contracts[0];
  put(
    "contract-costs",
    Object.fromEntries(
      choices.map(d => [
        `${d.venue}.${d.symbol}`,
        { ...cost.cost_schedule[0].values, cost_schedule: cost.cost_schedule, slippage_ticks: "0" },
      ]),
    ),
  );
  const query: TerminalContext["query"] = async (...[method]) => ({
    ...snapshot,
    research_strategy: {
      packages: [],
      deliveries: [],
      next_after: "",
      ...(method === "research.strategy.read"
        ? { package: candidate }
        : method === "research.strategy.delivery.read"
          ? { delivery }
          : {}),
    },
  });
  const trade = vi.fn(async (_method: string, _params?: Record<string, unknown>) => {});
  const context: TerminalContext = {
    snapshot,
    query,
    trade,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
    workspacePage: "backtest",
    workspaceParams: { strategy_delivery_id: delivery.object.id },
  };
  const view = render(
    <WorkspaceDraftScope store={store} namespace={ns}>
      <Backtest {...context} />
    </WorkspaceDraftScope>,
  );
  await screen.findByText("Fixed portfolio delivery · explicit-portfolio-target");
  expect((screen.getByRole("button", { name: "下一步" }) as HTMLButtonElement).disabled).toBe(true);
  expect(screen.getByRole("region", { name: "固定组合交付回测" }).textContent).toContain(
    "全部合约、单位和周期",
  );
  const complete = { ...snapshot, datasets: choices };
  view.rerender(
    <WorkspaceDraftScope store={store} namespace={ns}>
      <Backtest {...context} snapshot={complete} />
    </WorkspaceDraftScope>,
  );
  await waitFor(() =>
    expect((screen.getByRole("button", { name: "下一步" }) as HTMLButtonElement).disabled).toBe(
      false,
    ),
  );
  fireEvent.click(screen.getByRole("button", { name: "下一步" }));
  const checkbox = screen.getByLabelText(
    "使用固定权重组合政策（仅完整普通日线）",
  ) as HTMLInputElement;
  expect(checkbox.checked).toBe(true);
  expect(checkbox.disabled).toBe(true);
  for (const [label, value] of [
    ["原合约名义权重 SHFE/rb2610", "0.4"],
    ["原合约名义权重 SHFE/hc2610", "-0.2"],
    ["组合总绝对名义权重预算", "0.8"],
    ["组合净名义权重预算", "0.3"],
  ]) {
    const field = screen.getByLabelText(label) as HTMLInputElement;
    expect(field.value).toBe(value);
    expect(field.disabled || Boolean(field.closest("fieldset")?.disabled)).toBe(true);
  }
  expect(screen.queryByLabelText("快均线")).toBeNull();
  const funds = screen.getByLabelText("初始资金") as HTMLInputElement;
  expect(funds.disabled).toBe(false);
  fireEvent.change(funds, { target: { value: "200000" } });
  await waitFor(() =>
    expect((screen.getByRole("button", { name: "下一步" }) as HTMLButtonElement).disabled).toBe(
      false,
    ),
  );
  expect(trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "下一步" }));
  expect(screen.getByText("固定组合名义政策").closest("dl")!.textContent).toContain(
    "SHFE/hc2610：-0.2",
  );
  fireEvent.click(screen.getByRole("button", { name: "开始回测" }));
  await waitFor(() => expect(trade).toHaveBeenCalledTimes(1));
  expect(trade.mock.calls[0][0]).toBe("backtest.submit");
  expect(trade.mock.calls[0][1]).toMatchObject({
    id: delivery.target_task_id,
    deposit: "200000",
    strategies: [candidate.definition.portfolio!.strategy],
    strategy_use: { package: candidate.object, delivery: delivery.object },
  });
  store.dispose();
});
