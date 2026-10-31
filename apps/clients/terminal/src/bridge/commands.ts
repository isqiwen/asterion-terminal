import type {
  ResourceObject,
  PerformanceAssumptions,
  FuturesContract,
  LogicObservation,
  ResearchStrategyUse,
  StatisticsRequest,
  EvaluationRequest,
  ResearchSampleEvaluation,
  ResearchSampleInput,
  MarketObservation,
  MaterialRequest,
} from "./client";
// The current UI-to-Native contract. JSON is encoded only by the bridge.
// Command names and parameters stay correlated through host and component callbacks.
import type {
  BacktestExperiment,
  TaskOwnerRef,
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
  | { action: "live_revoke" }
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
  "research.logic.list": { after: string; limit: number };
  "research.logic.read": { id: string };
  "research.logic.register": {
    id: string;
    definition: { id: string; name: string; purpose: string; fast: number; slow: number };
  };
  "research.logic.deprecate": { id: string; definition: ResourceObject; reason: string };
  "research.logic.examples": { after: string; limit: number; definition_id: string };
  "research.logic.example.read": { id: string };
  "research.logic.example.save": {
    id: string;
    example: {
      id: string;
      definition: ResourceObject;
      name: string;
      use_scope: string;
      fictional: true;
      observations: LogicObservation[];
    };
  };
  "research.logic.trial": { id: string; example: ResourceObject };
  "research.strategy.list": { after: string; limit: number };
  "research.strategy.read": { id: string };
  "research.strategy.logic.freeze": {
    id: string;
    registration: {
      id: string;
      name: string;
      purpose: string;
      limitations: string;
      source: ResourceObject;
      contract: FuturesContract;
      interval_minutes: number;
      strategy: StrategyDefinition;
    };
  };
  "research.strategy.freeze": {
    id: string;
    registration: {
      id: string;
      name: string;
      purpose: string;
      limitations: string;
      source: ResourceObject;
    };
  };
  "research.strategy.deprecate": { id: string; package: ResourceObject; reason: string };
  "research.strategy.delivery.create": {
    id: string;
    delivery: { id: string; package: ResourceObject; target_task_id: string };
  };
  "research.strategy.delivery.read": { id: string };
  "research.strategy.delivery.list": { after: string; limit: number; package_id: string };

  "research.variable.check": { expression: string; window: number };
  "research.variable.read": { id: string };
  "research.variable.list": { after: string; limit: number };
  "research.variable.register": {
    id: string;
    registration: { id: string; name: string; purpose: string; expression: string; window: number };
  };
  "resource.catalogue": {
    domain: "data" | "research";
    kind: string;
    after: string;
  };
  "resource.inspect": { object: ResourceObject };
  "runtime.snapshot": Record<string, never>;
  "data.download.budget.configure": {
    source: string;
    token: string;
    requests_per_minute: number;
    provider?: string;
  };
  "task.page": { owner: TaskOwnerRef; before_sequence: number };
  "data.credentials.save": {
    provider: string;
    credential: string;
    remember: boolean;
    requests_per_minute: number;
  };
  "data.credentials.clear": { provider: string };
  "data.credentials.verify": { provider: string };
  "trading.identity.prepare": Record<string, never>;
  "trading.connections.save": {
    id: string;
    name: string;
    host: string;
    port: number;
    instance: string;
    fingerprint: string;
    revision: string;
  };
  "trading.connections.remove": { id: string; revision: string };
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
  "node.research.local.create": { plugins: string[] };
  "native.plugins.inspect": Record<string, never>;
  "native.plugins.preview": { path: string };
  "native.plugins.install": { path: string; sha256: string };
  "native.plugins.uninstall": { file: string; sha256: string };
  "node.plugins.configure": { service: string; revision: string; plugins: string[] };
  "research.sample.submit": {
    id: string;
    owner: TaskOwnerRef;
    data_owner: { node_id: string; service_id: string };
    selection: ResearchSampleInput["request"]["selection"];
    use_scope: string;
    mode: "post_hoc";
    evaluation: ResearchSampleEvaluation;
  };
  "evaluation.submit": EvaluationRequest & { id: string; owner: TaskOwnerRef };
  "statistics.submit": StatisticsRequest & {
    id: string;
    owner: TaskOwnerRef;
  };
  "node.research.local.open": Record<string, never>;
  "node.data.local.open": Record<string, never>;
  "lab.source.configure": { provider: string };
  "recorder.endpoint.save": { host: string; port: number };
  "recorder.endpoint.clear": Record<string, never>;
  "research.repository.save": { path: string };
  "research.repository.clear": Record<string, never>;
  "lab.endpoint": Record<string, never>;
  "backtest.margins.preview": Record<string, never>;
  "backtest.submit": RiskParameters & {
    performance_assumptions?: PerformanceAssumptions;
    id: string;
    deposit: string;
    strategies: StrategyDefinition[];
    holdout_from: string;
    walk_forward: BacktestExperiment["walk_forward"];
    contracts: ContractCostSelection[];
    settlement_margins?: SettlementMargin[];
    margin_snapshot?: string;
    strategy_use?: ResearchStrategyUse;
  };
  "backtest.reuse": ReuseParameters & {
    source_input_sha256: string;
    slippage_ticks?: number;
    fee_multiplier?: string;
    strategy_use?: ResearchStrategyUse;
  };
  "backtest.budget.review": {
    id: string;
    input_sha256: string;
    result_digest: string;
    limits: { margin: string | null; gross_notional: string | null };
  };
  "factor.submit": {
    id: string;
    variable_definition?: import("./client").ResourceObject;
    retain_observations?: boolean;
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
  "data.history.retirement.preview": { id: string };
  "data.tables.retirement.preview": { id: string };
  "data.tables.retirement.execute": { id: string; preview_digest: string };
  "data.tables.retirements": { after: string };
  "data.history.retirement.execute": { id: string };
  "data.history.retirements": { after: string };
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
    connection_id: string;
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
  "task.retirement.preview": { id: string; owner: TaskOwnerRef };
  "task.retirement.execute": { id: string; owner: TaskOwnerRef };
  "task.action": { owner: TaskOwnerRef; id: string; action: "cancel" | "retry" };
  "task.result": { owner: TaskOwnerRef; id: string };
  "task.result.read": { owner: TaskOwnerRef; id: string };
  "task.metadata.read": { owner: TaskOwnerRef; id: string };
  "task.metadata.edit": {
    owner: TaskOwnerRef;
    id: string;
    expected_revision: number;
    name: string;
    note: string;
  };
  "evaluation.report.export": {
    id: string;
    owner: TaskOwnerRef;
    version: string;
    format: "html" | "csv";
    path: string;
  };
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
  "market.connect": { password: string; instruments: InstrumentKey[] };
  "market.credentials.save": { account: string; password: string; auth_code: string };
  "market.credentials.clear": { account: string };
  "market.catalog": { account: string; password: string; auth_code: string };
  "market.subscribe": { instruments: InstrumentKey[] };
  "market.disconnect": Record<string, never>;
  "market.minutes": { venue: string; symbol: string };
  "market.observation.preview": { venue: string; symbol: string };
  "data.observation.admit": { id: string; source: MarketObservation["reference"] };
  "data.observation.execute": { id: string };
  "data.observation.read": { id: string };
  "data.material.sources": Record<string, never>;
  "data.material.submit": MaterialRequest;
  "data.material.task": { id: string };
  "data.material.read": { id: string };
  "system.backup.preview": { operation_id: string; services: string[]; destination: string };
  "system.backup.save": {
    operation_id: string;
    services: string[];
    destination: string;
    preview_digest: string;
  };
  "system.backup.inspect": { backup: string };
  "system.backup.resume.preview": { backup: string };
  "system.backup.restore": { backup: string; destination: string };
  "system.backup.cancel": Record<string, never>;
  "live.create": RiskParameters & {
    account: string;
    max_price_deviation: string;
    contracts: InstrumentKey[];
  };
  "live.policy.configure": RiskParameters &
    AccountCommand & {
      max_price_deviation: string;
      contracts: InstrumentKey[];
    };
  "live.open": { account: string };
  "live.connect": { account: string };
  "live.disconnect": { account: string };
  "live.costs": { account: string };
  "live.act": AccountCommand & LiveAction;
  "live.close": { account: string };
  "node.action": { service: string; action: "start" | "stop" | "restart" };
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
export type AccountMethod = Extract<TerminalCommand, `live.${string}`>;
type WithoutAccount<P> = P extends { account: string } ? Omit<P, "account"> : never;
export type AccountCall = {
  [M in AccountMethod]: Record<string, never> extends WithoutAccount<CommandParams[M]>
    ? [method: M, params?: Record<string, never>]
    : [method: M, params: WithoutAccount<CommandParams[M]>];
}[AccountMethod];
export type AccountRun = (...args: AccountCall) => Promise<boolean>;
