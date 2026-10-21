// The current UI-to-Native contract. JSON is encoded only by the bridge.
// Command names and parameters stay correlated through host and component callbacks.
import type {
  BacktestExperiment,
  CostVersion,
  DailyTable,
  FactorExperiment,
  HistoryUpdateQuery,
  SettlementMargin,
  StrategyDefinition,
  TableRefreshQuery,
} from "./client";

type InstrumentKey = { venue: string; symbol: string };
type RiskParameters = {
  max_order_quantity: string;
  max_gross_quantity: string;
  max_working_orders: string;
};
type AccountCommand = {
  account: string;
  request_id: string;
  account_id: string;
  policy_revision: string;
};
export type LiveAction =
  | { action: "live_authorize"; user_id: string }
  | { action: "live_revoke" | "strategy_stop" }
  | { action: "live_resolve"; order_id: string }
  | { action: "cancel"; order_id: string }
  | {
      action: "submit";
      order_id: string;
      venue: string;
      symbol: string;
      side: "buy" | "sell";
      offset: "open" | "close_today" | "close_yesterday" | "close";
      quantity: string;
      price: string;
    };
type ReuseParameters = {
  id: string;
  source_id: string;
  source_result_digest: string;
  mode: "fixed" | "snapshot";
  snapshot: string;
};
type DatasetRange = { daily: DailyTable; begin_day: string; end_day: string };
type ContractCostSelection = {
  venue: string;
  cost_schedule: CostVersion[];
  slippage_ticks: number;
} & ({ symbol: string } | { product: string });
export type ComparisonMatrix = {
  language: string;
  reference_id: string;
  columns: { id: string; methodology: string; reasons: string[] }[];
  rows: {
    label: string;
    cells: {
      text: string;
      state: string;
      metrics: { label: string; text: string; state: string }[];
    }[];
  }[];
};

export type CommandParams = {
  "runtime.snapshot": Record<string, never>;
  "data.download.budget.configure": {
    source: string;
    token: string;
    requests_per_minute: number;
    provider?: string;
  };
  "task.page": { before_sequence: number };
  "data.credentials.save": {
    provider: string;
    credential: string;
    remember: boolean;
    requests_per_minute: number;
  };
  "data.credentials.clear": { provider: string };
  "data.credentials.verify": { provider: string };
  "ctp.connections.save": {
    id: string;
    name: string;
    revision: string;
    broker_id: string;
    user_id: string;
    app_id: string;
    trade_front: string;
    market_front: string;
  };
  "ctp.connections.remove": { id: string; revision: string };
  "ctp.connections.market": { id: string };
  "node.data_tasks.local.create": { plugins: string[] };
  "native.plugins.inspect": Record<string, never>;
  "native.plugins.preview": { path: string };
  "native.plugins.install": { path: string; sha256: string };
  "native.plugins.uninstall": { file: string; sha256: string };
  "node.plugins.configure": { id: string; service: string; revision: string; plugins: string[] };
  "node.update": { id: string; service: string; revision: string };
  "node.data_tasks.local.open": Record<string, never>;
  "node.data_tasks.attach": { id: string; service: string };
  "backtest.margins.preview": Record<string, never>;
  "backtest.submit": RiskParameters & {
    id: string;
    deposit: string;
    strategies: StrategyDefinition[];
    holdout_from: string;
    walk_forward: BacktestExperiment["walk_forward"];
    contracts: ContractCostSelection[];
    settlement_margins?: SettlementMargin[];
    margin_snapshot?: string;
  };
  "backtest.reuse": ReuseParameters & { source_input_sha256: string; slippage_ticks?: number };
  "backtest.budget.review": {
    id: string;
    input_sha256: string;
    result_digest: string;
    limits: { margin: string | null; gross_notional: string | null };
  };
  "factor.submit": {
    id: string;
    series:
      | { kind: "bars" }
      | {
          kind: "tables";
          snapshot: string;
          daily_source: string;
          calendar_source: string;
          end_day: string;
          ranges: { begin_day: string; instruments: string[] }[];
        };
    factor: FactorExperiment["factor"];
    lookbacks: number[];
    horizon: number;
    evaluation: FactorExperiment["evaluation"];
    liquidity?: FactorExperiment["liquidity"];
  };
  "factor.reuse": ReuseParameters & { experiment: FactorExperiment; lookbacks?: number[] };
  "data.minutes.page": {
    id: string;
    offset: number;
    limit: number;
    start: string;
    end: string;
    include_macd: boolean;
  };
  "data.minutes.range": {
    dataset_id: string;
    daily: DailyTable;
    begin_day: string;
    end_day: string;
    session_rules_version: number;
  };
  "data.minutes.evidence": { dataset_id: string; offset: number; limit: number };
  "data.download.minutes.submit": {
    id: string;
    source: string;
    contract_id: string;
    interval_minutes: number;
    requests_per_minute: number;
    token: string;
    snapshot: string;
    cutoff_ns: string;
  };
  "data.datasets": { venue: string; product: string; contract_id: string; source: string };
  "data.dataset.saved": Record<string, never>;
  "data.dataset.save": { name: string };
  "data.dataset.use": { id: string };
  "data.coverage": {
    venue: string;
    product: string;
    contract_id: string;
    source: string;
    daily: DailyTable | null;
  };
  "data.history.usage": { id: string };
  "data.download.update.plan": HistoryUpdateQuery;
  "data.download.update.submit": {
    id: string;
    query: HistoryUpdateQuery;
    plan_id: string;
    token: string;
  };
  "data.tables.refresh.plan": TableRefreshQuery;
  "data.tables.refresh.submit": {
    id: string;
    query: TableRefreshQuery;
    plan_id: string;
    token: string;
  };
  "data.tables.update.submit": {
    id: string;
    provider: string;
    tables: string[];
    begin_day: string;
    end_day: string;
    requests_per_minute: number;
    token: string;
  };
  "data.tables.coverage": Record<string, never>;
  "data.tables.page": {
    table: string;
    source: string;
    exchange: string;
    period: string;
    offset: number;
    limit: number;
  };
  "data.tables.products": { snapshot: string; contracts_source: string };
  "data.tables.listed_contracts": {
    snapshot: string;
    contracts_source: string;
    exchange: string;
    product: string;
  };
  "data.tables.daily": {
    daily: DailyTable;
    contract: string;
    period: string;
    offset: number;
    limit: number;
  };
  "data.tables.contracts": { snapshot: string; source: string; exchange: string; product: string };
  "data.dataset.select": DatasetRange & { source_dataset_ids: string[] };
  "data.dataset.series": DatasetRange & { source_dataset_ids: string[] };
  "data.dataset.daily": DatasetRange & { contract_id: string };
  "data.dataset.product": DatasetRange & { product: string };
  "data.dataset.remove": { venue: string; symbol: string };
  "data.dataset.clear": Record<string, never>;
  "task.action": { id: string; action: "cancel" | "retry" };
  "task.result": { id: string };
  "task.result.read": { id: string };
  "task.metadata.read": { id: string };
  "task.metadata.edit": { id: string; expected_revision: number; name: string; note: string };
  "task.evidence.export": { id: string; path: string };
  "task.comparison.export": {
    expected: {
      id: string;
      result_digest: string;
      metadata_revision: number;
      input_sha256: string | null;
    }[];
    matrix: ComparisonMatrix;
    path: string;
  };
  "task.portfolio.analyze": {
    expected: { id: string; result_digest: string }[];
    training_end: string;
  };
  "task.quantstats.analyze": { id: string; result_digest: string };
  "market.local": Record<string, never>;
  "market.attach": { id: string; service: string };
  "market.connect": { password: string; instruments: InstrumentKey[] };
  "market.credentials.save": { account: string; password: string; auth_code: string };
  "market.credentials.clear": { account: string };
  "market.catalog": { account: string; password: string; auth_code: string };
  "market.subscribe": { instruments: InstrumentKey[] };
  "market.disconnect": Record<string, never>;
  "market.minutes": { venue: string; symbol: string };
  "node.initializer.export": { path: string };
  "node.key.prepare": { id: string };
  "node.firewall.inspect": {
    id: string;
    host: string;
    ssh_port: string;
    username: string;
    key_source: string;
    private_key: string;
    known_hosts: string;
    agent_port: string;
    firewall_port: string;
    firewall_action: string;
  };
  "node.firewall.apply": { token: string; private_key: string };
  "node.service_firewall": { id: string; service: string; action: string; token: string };
  "live.create": RiskParameters & {
    account: string;
    node: string;
    service: string;
    environment: "real" | "simulation";
    max_price_deviation: string;
    contracts: InstrumentKey[];
  };
  "live.policy.configure": RiskParameters &
    AccountCommand & {
      risk_artifact: string;
      max_price_deviation: string;
      contracts: InstrumentKey[];
    };
  "live.open": { account: string };
  "live.connect": { account: string; password: string; auth_code: string };
  "live.disconnect": { account: string };
  "live.costs": { account: string };
  "live.act": AccountCommand & LiveAction;
  "live.strategy.start": AccountCommand & InstrumentKey & { strategy: StrategyDefinition };
  "live.close": { account: string };
  "node.bootstrap": {
    id: string;
    host: string;
    ssh_port: string;
    username: string;
    key_source: string;
    private_key: string;
    known_hosts: string;
    agent_port: string;
  };
  "node.connect": { id: string };
  "node.identity.replace": {
    id: string;
    host: string;
    ssh_port: string;
    username: string;
    key_source: string;
    private_key: string;
    known_hosts: string;
  };
  "node.local": Record<string, never>;
  "node.disconnect": { id: string };
  "node.deploy": { id: string; service: string; port: string; kind: string };
  "node.action": { id: string; service: string; action: "start" | "stop" | "remove" | "restart" };
};
export type TerminalCommand = Exclude<keyof CommandParams, "runtime.snapshot">;
export type CommandCall<K extends keyof CommandParams = TerminalCommand> = {
  [M in K]: Record<string, never> extends CommandParams[M]
    ? [method: M, params?: CommandParams[M]]
    : [method: M, params: CommandParams[M]];
}[K];
export type CommandHandler<R = void, K extends keyof CommandParams = TerminalCommand> = (
  ...args: CommandCall<K>
) => Promise<R>;

// A trading panel is bound to one connection; its adapter supplies that account.
export type AccountMethod = Extract<TerminalCommand, `live.${string}`> | "market.catalog";
type WithoutAccount<P> = P extends { account: string } ? Omit<P, "account"> : never;
export type AccountCall = {
  [M in AccountMethod]: Record<string, never> extends WithoutAccount<CommandParams[M]>
    ? [method: M, params?: Record<string, never>]
    : [method: M, params: WithoutAccount<CommandParams[M]>];
}[AccountMethod];
export type AccountRun = (...args: AccountCall) => Promise<boolean>;
