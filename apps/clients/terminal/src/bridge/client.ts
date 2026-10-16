import { BackendError } from "../i18n/errors";
import { translate, type MessageValues, getLocale } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { desktop, save } from "./desktop";
import type { PriceIncrementChange } from "../ui/contractUnits";
import { nativeDesktop } from "./desktop";
export type FuturesContract = {
  venue: string;
  symbol: string;
  product: string;
  delivery_month: string;
  currency: string;
  price_increment: string;
  quantity_increment: string;
  multiplier: string;
};
// Where a dataset's settlement prices, trading days, open interest and units
// are read: the market-wide daily table, its calendar and the contract list
// under one snapshot.
export type DailyTable = {
  snapshot: string;
  daily_source: string;
  calendar_source: string;
  contracts_source: string;
};
// Bars resolved by Data from immutable provider downloads.
export type DatasetSelection = {
  source_dataset_ids: string[];
  daily: DailyTable;
  begin_day: string;
  end_day: string;
  // With the units the data give it: its price increment is that of the first
  // bar's day, and changes on the days listed.
  contract: FuturesContract;
  price_increment_changes: PriceIncrementChange[];
  venue: string;
  symbol: string;
  revision: string;
  source: string;
  interval_minutes: number;
  count: number;
  days: number;
  first_day: string;
  last_day: string;
  first_timestamp_ns: string;
  last_timestamp_ns: string;
  last_close: string;
  // The trading days it settled on without bars: it did not trade, or its
  // bars are missing. They are among `days`.
  uncovered_days: string[];
};
// Month contracts of one product selected as its dominant series: the months
// that trade (each also a selected dataset) and the day each becomes dominant.
// factor scales that month's raw prices for the strategy's signal.
export type DatasetSeries = {
  venue: string;
  product: string;
  symbols: string[];
  rolls: { trading_day: string; symbol: string; factor: string }[];
  // Trading days on which the dominant month can be compared with a later one.
  terms: number;
};
export type HistoryVersionEvidence = {
  dataset_id: string;
  acquired_at_ns: string;
  source_availability: "unknown";
};
// A dataset recorded in experiment evidence; bars and days are omitted.
export type DatasetEvidence = {
  history_evidence: HistoryVersionEvidence[];
  revision: string;
  source: string;
  source_dataset_ids: string[];
  daily: DailyTable;
  // When the most recent of the daily partitions read was acquired.
  daily_acquired_at_ns: string;
  interval_minutes: number;
  contract: FuturesContract;
  price_increment_changes: PriceIncrementChange[];
};
export type ContractCosts = {
  margin_per_lot: string;
  open_fee: string;
  close_today_fee: string;
  close_yesterday_fee: string;
  // Notional rates: price x quantity x multiplier x rate.
  margin_rate: string;
  open_fee_rate: string;
  close_today_fee_rate: string;
  close_yesterday_fee_rate: string;
};
export type CostVersion = { effective_from: string; source: string; values: ContractCosts };
// One portfolio contract: its terms, costs and latest mark.
export type AccountContract = {
  contract: FuturesContract;
  costs: ContractCosts;
  cost_schedule: CostVersion[];
  mark: string;
};
export type RiskLimits = {
  max_order_quantity: string;
  max_gross_quantity: string;
  max_working_orders: number;
};
// The account a backtest ends with.
export type BacktestAccount = {
  mode: "backtest";
  persistent: false;
  storage_state: "ready" | "recovery_required";
  balance: string;
  equity: string;
  available: string;
  margin: string;
  frozen: string;
  fees: string;
  realized: string;
  unrealized: string;
  cursor: number;
  total: number;
  timestamp_ns: string | null;
  contracts: AccountContract[];
  risk: RiskLimits;
  positions: {
    venue: string;
    symbol: string;
    side: "buy" | "sell";
    bucket: "today" | "yesterday";
    quantity: string;
    basis: string;
  }[];
  orders: {
    id: string;
    venue: string;
    symbol: string;
    side: "buy" | "sell";
    offset: string;
    quantity: string;
    limit_price: string;
    filled: string;
    state: string;
  }[];
  fills: {
    id: string;
    order_id: string;
    venue: string;
    symbol: string;
    quantity: string;
    price: string;
    fee: string;
  }[];
};
// A live CTP session; the broker reports the account, the session owns the
// execution chain (authorization, allowed contracts, risk, order records).
export type LiveSession = {
  mode: "live";
  account_id: string;
  policy_revision: string;
  risk_artifact: string;
  segment_count: number;
  capacity: {
    records_used: number;
    records_limit: number;
    bytes_used: number;
    bytes_limit: number;
  };
  broker: {
    front: string;
    broker_id: string;
    user_id: string;
    app_id: string;
  };
  risk: RiskLimits;
  // Bound on a limit price's distance from the broker's latest price.
  max_price_deviation: string;
  contracts: FuturesContract[];
  phase:
    | "disconnected"
    | "connecting"
    | "authenticating"
    | "logging_in"
    | "confirming"
    | "synchronizing"
    | "ready"
    | "error";
  error_code: number;
  trading_day: string;
  synchronized_ms: number;
  funds: null | {
    balance: string;
    available: string;
    margin: string;
    commission: string;
    close_profit: string;
    position_profit: string;
  };
  positions: {
    venue: string;
    symbol: string;
    side: "buy" | "sell";
    today: string;
    yesterday: string;
  }[];
  orders: {
    // Empty for orders this session did not place.
    id: string;
    broker_key: string;
    exchange_order_id: string;
    venue: string;
    symbol: string;
    side: "buy" | "sell";
    offset: string;
    quantity: string;
    filled: string;
    limit_price: string;
    status: "submitted" | "accepted" | "partially_filled" | "filled" | "cancelled" | "rejected";
    error_code: number;
  }[];
  trades: {
    id: string;
    order_id: string;
    venue: string;
    symbol: string;
    side: "buy" | "sell";
    offset: string;
    quantity: string;
    price: string;
    trading_day: string;
    trade_time: string;
  }[];
  // Lasts until revoked, a policy change or a service restart.
  authorization: null | { authorized_at_ms: number };
  // The current or last strategy run; while running it alone places orders.
  // It continues across connections and trading days.
  strategy: null | {
    id: string;
    venue: string;
    symbol: string;
    strategy: StrategyDefinition;
    // Completed bars the rule needs before it gives a target.
    warmup: number;
    state: "running" | "stopped";
    reason: string;
    started_ms: number;
    bars: number;
    bar_ms: number;
    // The position wanted in lots: positive long, negative short.
    target: string | null;
    orders: string[];
  };
  // Recorded orders the broker does not report; never resent.
  unconfirmed: { id: string; broker_key: string; trading_day: string }[];
  // The account's rates from the broker; costs only when ready.
  costs: {
    venue: string;
    symbol: string;
    state: "querying" | "ready" | "unavailable";
    error_code: number;
    queried_ms: number;
    costs: ContractCosts | null;
  }[];
  storage_state: "ready" | "recovery_required";
  connection_state?: "disconnected";
};
export type FirewallPlan = {
  id: string;
  host: string;
  service?: string;
  transport: "ssh" | "agent";
  token: string;
  source: string;
  port: number;
  backend: string;
  state: string;
  can_apply: boolean;
  rule: string;
  action: "allow" | "remove";
  verification: string;
  // SSH-inspected plans only: target platform, the source address the node
  // observed for this Terminal, and whether an Asterion-owned rule exists.
  os?: string;
  observed_source?: string;
  owned?: boolean;
};
export type TerminalCommand =
  | "node.agent.upgrade"
  | "node.agent.inspect"
  | "data.download.budget.configure"
  | "task.page"
  | "data.credentials.save"
  | "data.credentials.clear"
  | "data.credentials.verify"
  | "ctp.connections.save"
  | "ctp.connections.remove"
  | "ctp.connections.market"
  | "node.data_tasks.local.create"
  | "native.plugins.inspect"
  | "native.plugins.preview"
  | "native.plugins.install"
  | "native.plugins.uninstall"
  | "node.plugins.configure"
  | "node.update"
  | "node.data_tasks.local.open"
  | "node.data_tasks.attach"
  | "backtest.margins.preview"
  | "backtest.submit"
  | "backtest.reuse"
  | "backtest.budget.review"
  | "factor.submit"
  | "factor.reuse"
  | "data.minutes.page"
  | "data.minutes.range"
  | "data.minutes.evidence"
  | "data.download.minutes.submit"
  | "data.datasets"
  | "data.dataset.saved"
  | "data.dataset.save"
  | "data.dataset.use"
  | "data.coverage"
  | "data.history.usage"
  | "data.download.update.plan"
  | "data.download.update.submit"
  | "data.tables.refresh.plan"
  | "data.tables.refresh.submit"
  | "data.tables.update.submit"
  | "data.tables.coverage"
  | "data.tables.page"
  | "data.tables.products"
  | "data.tables.listed_contracts"
  | "data.tables.daily"
  | "data.tables.contracts"
  | "data.dataset.select"
  | "data.dataset.series"
  | "data.dataset.daily"
  | "data.dataset.product"
  | "data.dataset.remove"
  | "data.dataset.clear"
  | "task.action"
  | "task.result"
  | "task.result.read"
  | "task.metadata.read"
  | "task.metadata.edit"
  | "task.evidence.export"
  | "task.comparison.export"
  | "market.local"
  | "market.attach"
  | "market.connect"
  | "market.credentials.save"
  | "market.credentials.clear"
  | "market.catalog"
  | "market.subscribe"
  | "market.disconnect"
  | "market.minutes"
  | "node.initializer.export"
  | "node.key.prepare"
  | "node.firewall.inspect"
  | "node.firewall.apply"
  | "node.service_firewall"
  | "live.create"
  | "live.policy.configure"
  | "live.open"
  | "live.connect"
  | "live.disconnect"
  | "live.costs"
  | "live.act"
  | "live.strategy.start"
  | "live.close"
  | "node.bootstrap"
  | "node.connect"
  | "node.identity.replace"
  | "node.local"
  | "node.disconnect"
  | "node.deploy"
  | "node.action";
export type ProgressObservation = {
  observed: boolean;
  pending: boolean;
  age_ms: number;
};
export type ExecutionHealth = {
  io: ProgressObservation;
  state: ProgressObservation;
  persistence: ProgressObservation;
  initialization: ProgressObservation;
  command: ProgressObservation;
  business_ready: boolean;
};
export type NodeStatus = {
  id: string;
  host: string;
  port: number;
  state: "online" | "unreachable";
  last_heartbeat_ms: number;
  latency_ms: number;
  error: string;
  health: null | {
    instance_id: string;
    phase: "initializing" | "ready" | "recovery_required";
    failure: null | { code: string; message: string };
    execution: ExecutionHealth;
    worker_capacity: { limit: number; owned: number; reserved: number };
    resource_budget: null | {
      limit: { cpu_slots: number; memory_mib: number; io_slots: number };
      committed: { cpu_slots: number; memory_mib: number; io_slots: number };
      file_workers: number;
    };
    maintenance: boolean;
    pid: number;
    os: string;
    arch: string;
    version: string;
    uptime_ms: number;
    services: {
      revision: string;
      kind: "live" | "market" | "task" | "data";
      task_service: string;
      data_service: string;
      active_workers: number;
      resource_request: { cpu_slots: number; memory_mib: number; io_slots: number };
      plugin_artifacts: string[];
      id: string;
      artifact: string;
      port: number;
      state: string;
      desired_running: boolean;
      pid: number;
      restarts: number;
      error: string;
      health: string;
      execution: ExecutionHealth | null;
      last_heartbeat_ms: number;
      endpoint: string;
      directory: string;
    }[];
  };
};
export type LiveMarket = {
  watchlist: { venue: string; symbol: string }[];
  // Incremental poll bookkeeping (see pollSnapshot); absent in fixtures.
  subscription_set?: number;
  delta?: boolean;
  catalog: {
    revision?: number;
    omitted?: boolean;
    phase: string;
    error_code: string;
    diagnostic: string;
    trading_day: string;
    contracts: {
      venue: string;
      symbol: string;
      product: string;
      expiry: string;
      contract_id?: string;
      multiplier: number;
      price_tick: string;
      name: string;
    }[];
  };
  history: {
    stream_id: string;
    generation: string;
    available: boolean;
    interrupted: boolean;
    points: {
      venue: string;
      symbol: string;
      timestamp_ns: string;
      price: string;
      volume?: string;
    }[];
  };
  service: string;
  remote: boolean;
  host: string;
  port: number;
  instance_id: string;
  phase: string;
  error_code: number;
  transport_online: boolean;
  sequence: number;
  out_of_order: number;
  subscriptions: {
    venue: string;
    symbol: string;
    state: string;
    error_code: number;
    // Revision of this row's last change, for incremental polls.
    revision?: number;
    // Percent vs the latest observation >= 60 s earlier; null until available.
    change_1m_percent?: string | null;
    quote: null | {
      last: string | null;
      bid: string | null;
      ask: string | null;
      previous_settlement: string | null;
      open: string | null;
      average_price?: string | null;
      previous_close: string | null;
      open_interest_change: string | null;
      upper_limit: string | null;
      lower_limit: string | null;
      high: string | null;
      low: string | null;
      open_interest: string | null;
      bid_levels: { price: string | null; quantity: number | null }[];
      ask_levels: { price: string | null; quantity: number | null }[];
      bid_quantity: number;
      ask_quantity: number;
      volume: number;
      source_ms: number;
      received_ms: number;
      trading_day: string;
      action_day: string;
      update_time: string;
    };
  }[];
};
// Current trading-day minute bars observed by the market service.
export type IntradaySeries = {
  // The market service that observed them, as the snapshot names it.
  service: string;
  instance_id: string;
  venue: string;
  symbol: string;
  trading_day: string;
  first_observation_ms: number;
  interrupted: boolean;
  previous_settlement: string | null;
  bars: {
    start_ms: number;
    open: string;
    high: string;
    low: string;
    close: string;
    volume: number;
    average_price: string | null;
    open_interest: string | null;
  }[];
};
export type FactorLiquidityRule = {
  minimum_history: number;
  minimum_volume: string;
  minimum_open_interest: string;
};
export type FactorMembershipReason =
  | "MEMBERSHIP_ELIGIBLE"
  | "MEMBERSHIP_UNOBSERVED"
  | "MEMBERSHIP_INSUFFICIENT_HISTORY"
  | "MEMBERSHIP_UNKNOWN_VOLUME"
  | "MEMBERSHIP_UNKNOWN_OPEN_INTEREST"
  | "MEMBERSHIP_BELOW_VOLUME"
  | "MEMBERSHIP_BELOW_OPEN_INTEREST";
export type FactorResult = {
  memberships: { series_index: number; trading_day: string; reason: FactorMembershipReason }[];
  dataset_revision: string;
  lookback: number;
  horizon: number;
  input_count: number;
  purged_count: number;
  evaluation_warmup: number;
  selection_rule:
    "fixed" | "development_abs_spearman" | "rolling_fixed" | "rolling_development_abs_spearman";
  folds: {
    training_begin: number;
    training_end: number;
    validation_end: number;
    lookback: number;
    candidates: FactorResult["candidates"];
    development: FactorResult["partitions"][number];
    holdout: FactorResult["partitions"][number];
  }[];
  candidates: { lookback: number; sample_count: number; development_spearman: number | null }[];
  partitions: {
    name: "full_sample" | "development" | "holdout";
    begin_index: number;
    end_index: number;
    sample_count: number;
    // One series: its correlation over time. Several: the mean of their
    // cross-sections, and that mean over the cross-sections' deviation.
    pearson: number | null;
    spearman: number | null;
    pearson_ratio: number | null;
    spearman_ratio: number | null;
    // Several series: entry k - 1 is the mean label of the k highest
    // features and of the k lowest, over the `observations` that had 2k series.
    spreads: { highest: number; lowest: number; observations: number }[];
  }[];
  // One row per evaluated observation: samples for one series, cross-sections
  // for several; the other list is empty. `observed` and `label` are a
  // nanosecond timestamp for bars, the provider's trading date for daily closes.
  samples: {
    event_index: number;
    observed: string;
    label: string;
    value: number;
    forward_return: number;
  }[];
  cross_sections: {
    event_index: number;
    observed: string;
    label: string;
    // Null where fewer than three series had the observation.
    pearson: number | null;
    spearman: number | null;
    series: number;
    // Grouping order: descending feature, input order for ties (not IC ranks).
    members: { series_index: number; value: number; forward_return: number }[];
  }[];
};
export type HistoryConnectionSchema = {
  credential_label_en: string;
  credential_label_zh: string;
  credential_required: boolean;
  credential_max_length: number;
  remember_allowed: boolean;
  requests_per_minute_default: number;
  requests_per_minute_max: number;
};
// What was saved for one data provider (a data source plugin): a single
// credential used by all of its sources. The credential itself never leaves
// the core.
export type DataCredential = {
  provider: string;
  requests_per_minute: number;
  remember: boolean;
  credential_ready: boolean;
};
// A stored file the core could not read; kept for inspection.
export type UnreadableDataCredential = { provider: string; error: "unreadable" };
export type DataCredentialEntry = DataCredential | UnreadableDataCredential;
export type UnreadableCtpConnection = { id: string; name: string; error: "unreadable" };
// The saved credential of the provider a source belongs to, when it can be used.
export function savedCredential(
  snapshot: Snapshot | null,
  source: string,
): DataCredential | undefined {
  const provider = snapshot?.data?.sources.find(item => item.id === source)?.plugin_id;
  const entry = snapshot?.data_credentials?.find(item => item.provider === provider);
  return entry && !("error" in entry) && entry.credential_ready ? entry : undefined;
}
// One counter account as the broker issues it; passwords are never part of it.
export type CtpConnection = {
  id: string;
  name: string;
  broker_id: string;
  user_id: string;
  app_id: string;
  trade_front: string;
  market_front: string;
  revision: string;
  // Whether this account has its trading record; its counter details are then fixed.
  trading_record: boolean;
};
export type CtpConnectionEntry = CtpConnection | UnreadableCtpConnection;
export const readableCtpConnection = (entry: CtpConnectionEntry): entry is CtpConnection =>
  !("error" in entry);
// The one account that supplies market data.
export function marketCtpAccount(snapshot: Snapshot | null): CtpConnection | undefined {
  return (snapshot?.ctp_connections ?? [])
    .filter(readableCtpConnection)
    .find(item => item.id === snapshot?.ctp_market);
}
export type NativeHistorySource = {
  id: string;
  name: string;
  plugin_id: string;
  normalization: string;
  timezone: string;
  timestamp_semantics: string;
  venues: string[];
  intervals: number[];
  max_requests_per_minute: number;
  credential_required: boolean;
  connection?: HistoryConnectionSchema | null;
};
export type ExperimentMetadata = {
  revision: number;
  name: string;
  note: string;
  source_id: string;
  source_instance: string;
  source_result_digest: string;
  data_choice: string;
  snapshot: string;
};
export type TaskRecord = {
  error_code?: string; // absent when the task has no structured failure observation
  table_refresh?: TableRefreshTarget;
  metadata?: ExperimentMetadata;
  provider_artifact?: string;
  risk_artifact?: string;
  data_source?: string;
  history_dataset_id?: string;
  kind: "backtest" | "factor" | "minute_download" | "table_update";
  id: string;
  state:
    | "queued"
    | "running"
    | "cancel_requested"
    | "publishing"
    | "succeeded"
    | "failed"
    | "cancelled"
    | "interrupted";
  attempt: number;
  completed: number;
  total: number;
  error: string;
  result_digest: string;
  trading_day: string;
  instrument: string;
  source_name: string;
  minute_interval_minutes?: number;
  submission_sequence: number;
  submitted_at_ms: number;
  updated_at_ms: number;
};
export type SettlementExposure = {
  day: string;
  net_notional: string;
  gross_notional: string | null;
  model_margin: string | null;
  balance: string;
  equity: string;
  available: string | null;
  frozen: string | null;
};
export type PortfolioBudgetCheck = {
  used: string | null;
  limit: string | null;
  headroom: string | null;
  state: "unset" | "exceeded" | "within" | "unknown";
};
export type PortfolioExposure = {
  settlement_path: { scope: "recorded_settlement_points"; points: SettlementExposure[] };
  scope: "final_saved_account";
  contracts: {
    venue: string;
    symbol: string;
    mark: string;
    multiplier: string;
    quantity_increment: string;
    net_quantity: string;
    gross_quantity: string;
    net_notional: string;
    gross_notional: string;
    gross_notional_share: number | null;
    margin: string;
    cost_source: string;
    cost_effective_from: string;
    margin_per_lot: string;
    margin_rate: string;
    quantities_on_grid: boolean;
  }[];
  net_notional: string;
  gross_notional: string;
  gross_quantity: string;
  model_margin: string;
  recorded_margin: string;
  margin_difference: string;
  balance: string;
  equity: string;
  available: string;
  frozen: string;
  risk: {
    max_order_quantity: string;
    max_gross_quantity: string;
    max_working_orders: number;
    final_lot_headroom: string;
  };
};
export type PortfolioBudgetReview = {
  scope: "saved_account_and_settlement_points_review_only";
  settlement_checks: (SettlementExposure & {
    checks: Record<"gross_notional" | "margin", PortfolioBudgetCheck>;
  })[];
  settlement_summary: Record<
    "gross_notional" | "margin",
    { checked_points: number; unknown_points: number; exceeded_points: number | null }
  >;
  source: {
    task_id: string;
    input_sha256: string;
    result_digest: string;
    risk_artifact: string;
    task_instance: string;
  };
  limits: { gross_notional: string | null; margin: string | null };
  checks: Record<"gross_notional" | "margin", PortfolioBudgetCheck>;
  portfolio: PortfolioExposure;
};
export type AccountCheck = {
  metric: string;
  expected: string;
  recorded: string;
  difference: string;
  matches: boolean;
};
export type BacktestReconciliation = {
  state: "available" | "unsupported_input" | "missing_fill_days";
  model?: {
    venue: string;
    symbol: string;
    deposit: string;
    multiplier: string;
    cost_source: string;
    cost_effective_from: string;
    margin_per_lot: string;
    margin_rate: string;
    signed_notional: string;
    gross_notional: string;
  };
  final_checks: AccountCheck[];
  days: {
    day: string;
    opening_quantity: string;
    quantity_change: string;
    closing_quantity: string;
    settlement: string;
    opening_balance: string;
    gross: string;
    fees: string;
    expected_balance: string;
    recorded_balance: string;
    fills: {
      id: string;
      order_id: string;
      side: "buy" | "sell";
      quantity: string;
      price: string;
      fee: string;
    }[];
    checks: AccountCheck[];
  }[];
};
export type HistoricalMargins = {
  snapshot: string;
  contract: string;
  source: string;
  unit: "fraction";
  basis: "exchange_speculation";
  publication_known: false;
  unit_reference: string;
  phase_reference: string;
  basis_reference: string;
  days: {
    trading_day: string;
    day_start_rate: string;
    settlement_rate: string;
    revision_snapshot: string;
    content: string;
    object: string;
    provider_artifact: string;
    acquired_at_ns: string;
    day_start_url: string;
    settlement_url: string;
    day_start_sha256: string;
    settlement_sha256: string;
    day_start_updated: string;
    settlement_updated: string;
  }[];
};
export type SettlementMargin = { trading_day: string; rate: string; source: string };
export type BacktestResult = {
  fill_days?: { fill_id: string; trading_day: string }[];
  dataset_revision: string;
  // With several strategies: what the days before the holdout showed of each,
  // in the order they were given, and which one the rest of the result replays.
  candidates: { total_return: number; max_drawdown: number; sharpe: number | null }[];
  selected: number;
  // The rounds of a rolling comparison: what each one's training days showed
  // of every strategy, and the one followed from `first_day`, if any.
  folds: {
    first_day: string;
    candidates: { total_return: number; max_drawdown: number; sharpe: number | null }[];
    selected: number | null;
  }[];
  max_drawdown: string;
  account: BacktestAccount;
  equity: { timestamp_ns: string; equity: string; event: "trade" | "settlement" }[];
  settlements: {
    trading_day: string;
    timestamp_ns: string;
    balance: string;
    equity: string;
    realized: string;
    fees: string;
    contracts: {
      venue: string;
      symbol: string;
      price: string;
      position_quantity: string;
      gross_quantity?: string;
      margin?: string;
    }[];
    occupancy?: { margin: string; available: string; frozen: string };
    margin_revaluation?: {
      previous_rate: string;
      rate: string;
      margin_before: string;
      available_before: string;
    };
  }[];
};
export type ExperimentData = {
  count: number;
  first_timestamp_ns: string;
  last_timestamp_ns: string;
  first_day: string;
  last_day: string;
  interval_minutes: number;
  source: string;
  source_dataset_ids: string[];
};
export type BacktestExperiment = {
  historical_margins?: HistoricalMargins;
  settlement_margins?: SettlementMargin[];
  dataset_revision: string;
  // One strategy, or the several that were compared before `holdout_day`.
  strategies: StrategyDefinition[];
  holdout_day: string;
  // Or the several are compared by rolling: each round scores them over
  // `training_days` and the account follows the best over the
  // `validation_days` after.
  walk_forward: null | { training_days: number; validation_days: number };
  paper: {
    type: "historical_paper";
    deposit: string;
    risk: RiskLimits;
    // slippage_ticks: the price increments every fill of the contract gave up.
    contracts: {
      dataset: DatasetEvidence;
      cost_schedule: CostVersion[];
      slippage_ticks: number;
    }[];
  };
  // One range per contract, in contract order.
  data: ExperimentData[];
  // Present when contracts are traded as dominant series: the rolls of each,
  // naming contracts by index.
  series?: { trading_day: string; contract: number; factor: string }[][];
};
// One series a factor read: a contract's bars, a product's dominant series
// (the months it was read from, when each took over, and how many bars that
// made), or the daily closes of a published version.
export type FactorSeriesEvidence =
  | { kind: "bars"; dataset: DatasetEvidence; data: ExperimentData }
  | {
      kind: "dominant";
      months: { dataset: DatasetEvidence; data: ExperimentData }[];
      rolls: { trading_day: string; contract: number; factor: string }[];
      // Trading days that have a term point; `count` bars were read.
      terms: number;
      count: number;
    }
  | {
      // An instrument of the market-wide daily table: a contract, or a product
      // read as its dominant series (then with rolls, naming months by index).
      kind: "table";
      data: {
        instrument: string;
        snapshot: string;
        daily_source: string;
        calendar_source: string;
        begin_day: string;
        end_day: string;
        acquired_at_ns: string;
        months: {
          contract: string;
          count: number;
          first_day: string | null;
          last_day: string | null;
        }[];
        rolls: { trading_day: string; contract: number; factor: string }[];
        terms: number;
        count: number;
      };
    };
// What a factor's feature is: price momentum over a window, the mean over a
// window of the carry a dominant series' term structure shows each day, or
// the deviation of a window's returns.
export type FactorKind = "momentum" | "term_structure" | "volatility";
export type FactorExperiment = {
  liquidity: FactorLiquidityRule | null;
  factor: FactorKind;
  dataset_revision: string;
  series: FactorSeriesEvidence[];
  lookbacks: number[];
  horizon: number;
  evaluation:
    | { mode: "full_sample" }
    | { mode: "holdout"; split_index: number }
    | { mode: "walk_forward"; training_events: number; validation_events: number };
};
// Which sides of a contract a strategy may hold.
export type PositionSides = "both" | "long" | "short";
// What a strategy trades by: a rule over completed bars, the lots it holds
// while the rule takes a side, and the sides it may hold.
export type StrategyDefinition =
  | {
      quantity: string;
      sides: PositionSides;
      rule:
        | { kind: "moving_average"; fast: number; slow: number }
        | { kind: "breakout"; entry: number; exit: number }
        | { kind: "momentum"; lookback: number }
        | { kind: "reversion"; window: number; width: string };
    }
  // Ranks several contracts by a factor, holds `count` a side and sizes each
  // to the lots nearest `notional` instead of a quantity; backtests only.
  | {
      sides: PositionSides;
      rule: {
        kind: `cross_${FactorKind}`;
        // The lowest are held long and the highest short.
        reverse: boolean;
        lookback: number;
        rebalance: number;
        count: number;
        notional: string;
        // Bars of returns the held contracts' notional is split by; 0 for alike.
        volatility: number;
      };
    };
// What a backtest's closed positions say. A trade is one contract's position
// from flat to flat again; profits are before fees. The averages are null
// without a trade of their kind, the ratio of the two unless there are both.
export type HoldingSpan = {
  opened_day: string;
  ended_day: string;
  calendar_days: number;
  settlement_days: number;
};
export type BacktestTrades = {
  holding_summary?: {
    known: number;
    unknown: number;
    calendar_days: { minimum: number | null; mean: number | null; maximum: number | null };
    settlement_days: { minimum: number | null; mean: number | null; maximum: number | null };
  };
  open?: {
    venue: string;
    symbol: string;
    direction: "long" | "short";
    quantity: string;
    fees: string;
    as_of_day: string;
    holding: HoldingSpan | null;
  }[];
  by_contract: {
    venue: string;
    symbol: string;
    count: number;
    gross: string;
    fees: string;
    net: string;
  }[];
  closed: {
    venue: string;
    symbol: string;
    gross: string;
    fees: string;
    net: string;
    holding?: HoldingSpan | null;
  }[];
  gross: string;
  fees: string;
  net: string;
  open_fees: string;
  count: number;
  winning: number;
  losing: number;
  average_win: number | null;
  average_loss: number | null;
  payoff: number | null;
};
// How a backtest's result performed, from its day-end equity. The drawdown is
// a share of the peak it fell from. Yearly figures are scaled by the calendar
// time the days span and are null with fewer than 20 trading days.
export type PerformanceFigures = {
  trading_days: number;
  total_return: number;
  max_drawdown: number;
  winning_days: number;
  annual_return: number | null;
  annual_volatility: number | null;
  sharpe: number | null;
  calmar: number | null;
};
// Read-time attribution of the fixed settlement history, including open positions.
export type BacktestMonth = {
  first_day: string;
  last_day: string;
  trading_days: number;
  opening_equity: string;
  closing_equity: string;
  gross: string;
  fees: string;
  net: string;
  // Net divided by the original deposit, additive to the full result's return.
  return_contribution: number;
};
// The figures of all days and, where strategies were compared, of the days
// before and from the holdout; the holdout starts from the equity the
// development days ended with.
// `out_of_sample` and `folds` belong to a rolling comparison: the figures from
// the first round's validation days on, and of each round's validation days;
// a span that begins with an empty account has none.
export type BacktestPerformance = PerformanceFigures & {
  by_month: BacktestMonth[];
  out_of_sample: PerformanceFigures | null;
  folds: (PerformanceFigures | null)[];
  development: PerformanceFigures | null;
  holdout: PerformanceFigures | null;
};
export type TaskResult =
  | {
      id: string;
      kind: "backtest";
      input_sha256: string;
      task: TaskRecord;
      experiment: BacktestExperiment;
      result: BacktestResult;
      performance: BacktestPerformance;
      trades: BacktestTrades;
      reconciliation: BacktestReconciliation;
      portfolio: PortfolioExposure;
    }
  | {
      id: string;
      kind: "factor";
      task: TaskRecord;
      experiment: FactorExperiment;
      result: FactorResult;
    }
  | {
      id: string;
      kind: "minute_download";
      task: TaskRecord;
      experiment: {
        contract_id: string;
        interval_minutes: number;
        begin_ns: string;
        end_ns: string;
      };
      result: { directory: string; manifest_sha256: string; rows: number; pages: number };
    }
  | {
      id: string;
      kind: "table_update";
      task: TaskRecord;
      experiment: TableUpdate;
      result: TableUpdateResult;
    };
// One exchange and one period of a market-wide table: a trading day
// (YYYY-MM-DD), a year or "all", as the table is kept.
export type TablePartitionKey = { table: string; exchange: string; period: string };
// What an update of market-wide tables was asked to read. An empty begin day
// continues after what each exchange already holds; an empty end day reads
// through the last day that has ended.
export type TableRevision = TablePage["revision"];
export type TableRefreshTarget = {
  partition: TablePartitionKey;
  source: string;
  previous_snapshot: string;
  previous_content: string;
  provider_artifact: string;
};
export type TableRefreshQuery = {
  provider: string;
  source: string;
  table: string;
  exchange: string;
  period: string;
  previous_snapshot: string;
  requests_per_minute: number;
};
export type TableRefreshPlan = {
  id: string;
  query: TableRefreshQuery;
  previous: TableRevision;
  update: TableUpdate;
};
export type TableUpdate = {
  refresh?: TableRefreshTarget;
  provider: string;
  tables: string[];
  begin_day: string;
  end_day: string;
  requests_per_minute: number;
};
// What one update did, partition by partition. Stored partitions stay whatever
// became of the rest.
export type TableRefreshObservation = {
  kind: "explicit_refresh_response_observation";
  id: string;
  operation: string;
  data_instance: string;
  task_instance: string;
  task_id: string;
  attempt: number;
  provider: string;
  provider_artifact: string;
  selected_snapshot: string;
  selected_content: string;
  revision: TableRevision;
  normalized_object: string;
  format: string;
  body: string;
  response_sha256: string;
  response_bytes: number;
  received_at_ns: string;
  recorded_at_ns: string;
  unchanged: boolean;
  content_changed_from_selected: boolean;
};
export type TableUpdateResult = {
  refresh_observation?: TableRefreshObservation;
  refreshed_revision?: TableRevision;
  stored: number;
  unchanged: number;
  rejected: (TablePartitionKey & { reason: string })[];
  unpublished: number;
  unpublished_examples: TablePartitionKey[];
  // "table exchange" pairs that hold nothing yet while the update named no begin day.
  waiting: string[];
  snapshot: string;
};
// A table an installed data source provides.
export type TableSource = {
  table: string;
  schema: number;
  source: string;
  plugin_id: string;
  venues: string[];
};
// What the store holds for one exchange of one table. missing_days is null
// when there is no calendar to count against.
export type TableCoverageRow = {
  table: string;
  schema: number;
  source: string;
  exchange: string;
  partitions: number;
  rows: number;
  first_period: string;
  last_period: string;
  acquired_at_ns: string;
  missing_days: number | null;
  missing_examples: string[];
};
export type TableCoverage = { snapshot: string; sources: TableSource[]; rows: TableCoverageRow[] };
// A product of an exchange, as the contract lists of a snapshot hold it.
export type TableProduct = {
  exchange: string;
  product: string;
  contracts: number;
  first_listed: string;
  last_delisted: string;
};
export type TableListedContracts = {
  query: { snapshot: string; contracts_source: string; exchange: string; product: string };
  items: { contract_id: string; name: string; list_date: string; delist_date: string }[];
};
// A day a contract traded in the auction, or the last such day of the week,
// month, quarter or year it groups. Display data only.
export type TableCandle = {
  trading_day: string;
  open: string;
  high: string;
  low: string;
  close: string;
  volume: string;
  macd?: { diff: number; signal: number; histogram: number };
};
// A page of one contract's candles in the market-wide daily table.
export type TableDailyPage = {
  daily: DailyTable;
  contract: string;
  period: "day" | "week" | "month" | "quarter" | "year";
  total: number;
  offset: number;
  limit: number;
  candles: TableCandle[];
};
// One page of a stored partition. Cells are text as stored; null is a missing value.
export type TablePage = {
  latest_refresh_observation?: TableRefreshObservation | null;
  response_review?: null | {
    status: "consistent" | "differences" | "unknown";
    reason: string;
    response_fields: number;
    response_rows: number;
    normalized_rows: number;
    dated_rows: number;
    matched_rows: number;
    compared_cells: number;
    skipped_keys: string[];
    unknown_keys: string[];
    differences: {
      kind: string;
      key: string;
      field: string;
      raw: string | null;
      expected: string | null;
      actual: string | null;
    }[];
  };
  response_evidence?: null | {
    kind: "raw_response_body";
    format: string;
    received_at_ns: string;
    sha256: string;
    bytes: number;
    revision_snapshot: string;
    normalized_content: string;
    normalized_object: string;
    provider_artifact: string;
    body: string;
  };
  revision: TablePartitionKey & {
    sequence: number;
    schema: number;
    source: string;
    content: string;
    rows: number;
    bytes: number;
    acquired_at_ns: string;
    provider_artifact: string;
    snapshot: string;
  };
  columns: string[];
  rows: (string | null)[][];
  offset: number;
};
// A minute version against the market-wide daily table: of its days, the
// part the table holds (compared_first to compared_last, empty when none),
// the days the contract traded on in it, and those without minute bars.
export type HistoryCoverage = {
  minute_dataset_id: string;
  minute_source: string;
  interval_minutes: number;
  contract_id: string;
  minute_days: number;
  minute_first: string;
  minute_last: string;
  compared_first: string;
  compared_last: string;
  traded_days: number;
  uncovered: number;
  uncovered_days: string[];
  unknown_rows: number;
};
export type SessionClosureEvidence = {
  rule_id: string;
  version: number;
  exchange: string;
  first_day: string;
  last_day: string;
  published_day: string;
  source_url: string;
};
export type MinuteRangeEvidence = {
  session_rules_version: number;
  dataset_id: string;
  daily: DailyTable;
  begin_day: string;
  end_day: string;
  contract_id: string;
  minute_source: string;
  interval_minutes: number;
  request_begin_ns: string;
  request_end_ns: string;
  unknown_rows: number;
  request_session_coverage: "unverified";
  days: {
    trading_day: string;
    state:
      | "RANGE_OBSERVED"
      | "RANGE_UNCOVERED"
      | "RANGE_CLOSED"
      | "RANGE_UNKNOWN_CALENDAR"
      | "RANGE_UNKNOWN_DAILY"
      | "RANGE_NO_AUCTION"
      | "RANGE_UNKNOWN_LABELS"
      | "RANGE_CALENDAR_CONFLICT"
      | "RANGE_OFFICIAL_CLOSED"
      | "RANGE_RULE_CONFLICT";
    observed: { rows: number; first_ns: string; last_ns: string } | null;
    official_closure: SessionClosureEvidence | null;
    evening_closure: SessionClosureEvidence | null;
  }[];
};
export type MinuteEvidence = {
  dataset_id: string;
  begin_ns: string;
  end_ns: string;
  total_rows: number;
  unknown_rows: number;
  total_days: number;
  offset: number;
  limit: number;
  days: { trading_day: string; rows: number; first_ns: string; last_ns: string }[];
};
export type SavedNamedDataset = {
  id: string;
  name: string;
  // Each contract as history names it (SHFE/rb/2026-10): its units are the
  // data's and are read again when the dataset is used.
  selections: (Pick<
    DatasetSelection,
    "source_dataset_ids" | "daily" | "begin_day" | "end_day" | "revision"
  > & { contract: string })[];
};
export type HistoryReference = {
  kind: "download" | "backtest" | "factor" | "saved_dataset";
  id: string;
  name: string;
  roles: ("market" | "output")[];
};
export type HistoryUsage = {
  dataset_id: string;
  references: HistoryReference[];
  selected_roles: "market"[];
  disconnected_nodes: { names: string[]; error?: string };
  other_data_services: {
    node: string;
    service: string;
    checked: boolean;
    stopped?: boolean;
    references: HistoryReference[];
    error?: string;
  }[];
};
export type HistoryUpdateQuery = {
  dataset_id: string;
  daily: DailyTable | null;
  mode: "extend" | "repair";
  end_day: string;
  requests_per_minute: number;
};
export type HistoryUpdatePlan = {
  id: string;
  query: HistoryUpdateQuery;
  source: string;
  contract_id: string;
  interval_minutes: number;
  begin: string;
  end: string;
  missing_days: string[];
  session_rules_version: number;
  conflicts: {
    day: string;
    closure: SessionClosureEvidence;
    observed_minutes: boolean;
    calendar_open: boolean;
    daily_auction: boolean;
  }[];
};
export type HistoryDatasetRecord = {
  id: string;
  contract_id: string;
  source: string;
  revision: string;
  begin: string;
  end: string;
  interval_minutes: number;
  rows: number;
};
// The contracts a history source downloads, from its own data source's
// contract list under a snapshot of the market-wide tables. A download sends
// the snapshot and cutoff back: a contract still trading is fetched up to it.
export type TableContracts = {
  source: string;
  snapshot: string;
  exchange: string;
  product: string;
  cutoff_ns: string;
  items: {
    code: string;
    name: string;
    list_date: string;
    delist_date: string;
    multiplier: string | null;
    per_unit: string | null;
    trade_unit: string | null;
    quote_unit: string | null;
  }[];
};
export type HistoryBar = {
  macd?: { diff: number; signal: number; histogram: number };
  timestamp_ns: string;
  open: string;
  high: string;
  low: string;
  close: string;
  volume: string;
  amount: string;
  open_interest: string;
};
export type HistoryPage = {
  id: string;
  source: string;
  contract_id: string;
  interval_minutes: number;
  manifest_sha256: string;
  total_rows: number;
  matched_rows: number;
  offset: number;
  limit: number;
  first_ns: string;
  last_ns: string;
  begin_ns: string;
  end_ns: string;
  bars: HistoryBar[];
};
export type NativePluginInfo = {
  managed?: boolean;
  file: string;
  id: string;
  version: string;
  sha256: string;
  capabilities: { id: string; kind: string; version: number }[];
  state: "available" | "invalid";
  error: string;
};
export type Snapshot = {
  plugin_candidate?: NativePluginInfo;
  data_credentials?: DataCredentialEntry[];
  ctp_connections?: CtpConnectionEntry[];
  // The CTP account that supplies market data; trading accounts are independent.
  ctp_market?: string | null;
  // The last check of a provider's saved credential, per source.
  credential_verification?: null | {
    provider: string;
    checks: { source: string; scope: string; state: number }[];
  };
  history_page: HistoryPage | null;
  history_datasets?: HistoryDatasetRecord[];
  saved_datasets?: SavedNamedDataset[];
  history_coverage?: HistoryCoverage[];
  minute_evidence?: MinuteEvidence;
  minute_range?: MinuteRangeEvidence;
  history_update_plan?: HistoryUpdatePlan;
  history_usage?: HistoryUsage;
  historical_margins?: HistoricalMargins;
  table_coverage?: TableCoverage;
  table_page?: TablePage;
  table_refresh_plan?: TableRefreshPlan;
  table_products?: TableProduct[];
  table_listed_contracts?: TableListedContracts;
  table_daily?: TableDailyPage;
  table_contracts?: TableContracts;
  // Present when the core answered from its last snapshot because another operation was running.
  // Revision of the core's published state and when the core last refreshed it.
  revision: number;
  refreshed_at_ms: number;
  task_service: null | {
    connection_id: string;
    port: number;
    service: string;
    host: string;
    remote: boolean;
    online: boolean;
    error: string;
    tasks: TaskRecord[];
    before_sequence: number;
    next_before_sequence: number;
    failed_count: number;
    interrupted_count: number;
    capacity: {
      retained_tasks: number;
      active_used: number;
      active_reserved: number;
      uncommitted: number;
      active_limit: number;
    } | null;
  };
  data: null | {
    connection_id: string;
    port: number;
    service: string;
    host: string;
    remote: boolean;
    online: boolean;
    error: string;
    sources: NativeHistorySource[];
  };
  portfolio_budget_review?: PortfolioBudgetReview;
  research_export?: {
    name: string;
    sha256: string;
    bytes: number;
    path?: string;
    content?: string;
  };
  task_description?: TaskRecord;
  task_result: null | TaskResult;
  market: LiveMarket | null;
  intraday?: IntradaySeries;
  initializer?: {
    name: string;
    content: string;
  };
  ssh_key: {
    id: string;
    public_key: string;
  } | null;
  agent_program: null | {
    state: "isolated" | "not_installed" | "current" | "update_available" | "recovery_required";
    expected_digest: string;
    installed_digest: string;
    bundled_digest: string;
  };
  firewall_plan: FirewallPlan | null;
  nodes: NodeStatus[];
  protocol: 1;
  product: string;
  core: string;
  phase: "ready";
  asset: "futures";
  // The core's rule per exchange for assigning closes to today's and
  // yesterday's positions.
  close_policies: Record<string, "explicit_buckets" | "today_first" | "yesterday_first">;
  // Portfolio contracts selected for taskService, in order.
  datasets: DatasetSelection[];
  dataset_series: DatasetSeries[];
  native_plugins: null | {
    directory: string;
    managed_directory?: string;
    items: NativePluginInfo[];
  };
  plugins: {
    id: string;
    kind: string;
    state: string;
  }[];
  diagnostics: {
    succeeded: number;
    failed: number;
    log_failures: number;
    refresh_failures: number;
    refresh_failed: boolean;
  };
  // Open CTP trading accounts by account id; several may be open at once.
  live: Record<
    string,
    {
      session: LiveSession | null;
      connection: {
        transport: "local" | "tcp_tls";
        state: "connected" | "disconnected";
        session: string;
        port?: number;
        host?: string;
        reconnects: number;
        restarts: number;
        last_heartbeat_ms: number;
        latency_ms: number;
        health: null | {
          instance_id: string;
          version: string;
          uptime_ms: number;
          phase: "starting" | "degraded" | "ready" | "awaiting_input";
          execution: ExecutionHealth;
        };
      };
    }
  >;
};
async function call(
  method: "runtime.snapshot" | TerminalCommand,
  params: Record<string, unknown>,
): Promise<unknown> {
  const body = JSON.stringify({ version: 1, method, params });
  let raw: string;
  if (nativeDesktop) raw = await desktop().request(body);
  else if (import.meta.env.DEV) {
    const response = await fetch("/__asterion/api", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body,
    });
    if (!response.ok) throw new Error(t("本机 C++ 服务不可用，请重新启动终端"));
    raw = await response.text();
  } else throw new Error(t("请在 Asterion Terminal 桌面应用中打开"));
  const envelope = JSON.parse(raw);
  if (envelope.error)
    throw new BackendError(envelope.error.code ?? "operation_failed", envelope.error.message);
  return envelope.result;
}
function snapshotContract(result: unknown): Snapshot {
  const value = result as Snapshot;
  if (
    !value ||
    value.protocol !== 1 ||
    value.phase !== "ready" ||
    value.asset !== "futures" ||
    !Array.isArray(value.plugins)
  ) {
    throw new Error(t("本机核心返回了不支持的状态契约"));
  }
  return value;
}
export async function request(
  method: "runtime.snapshot" | TerminalCommand,
  params: Record<string, unknown> = {},
): Promise<Snapshot> {
  return snapshotContract(await call(method, params));
}
export function timestamp(ns: string | null): string {
  if (!ns) return "—";
  const date = new Date(Number(BigInt(ns) / 1000000n));
  return date.toLocaleString(getLocale(), { hour12: false, timeZone: "Asia/Shanghai" });
}
export async function exportLinuxInitializer(): Promise<boolean> {
  let destination = "";
  if (nativeDesktop) {
    const chosen = await save({
      title: t("导出 Linux 初始化脚本"),
      defaultPath: "initialize-linux.py",
    });
    if (!chosen) return false;
    destination = chosen;
  }
  const result = await request("node.initializer.export", { path: destination });
  if (!nativeDesktop) {
    if (!result.initializer) throw new Error(t("安装包未提供初始化脚本"));
    const url = URL.createObjectURL(
      new Blob([result.initializer.content], { type: "text/x-python;charset=utf-8" }),
    );
    const link = document.createElement("a");
    link.href = url;
    link.download = result.initializer.name;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  return true;
}
// Polls send the last revision they saw; an unchanged core returns no state.
export type SnapshotUnchanged = { unchanged: true; revision: number; refreshed_at_ms: number };
// With a held snapshot, the core returns only quote rows changed after the
// held revisions and omits an unchanged catalog; merge restores full state.
export async function pollSnapshot(
  since: number,
  held: Snapshot | null = null,
): Promise<Snapshot | SnapshotUnchanged> {
  const market = held?.market;
  const cursor =
    market?.subscription_set !== undefined && market.catalog.revision !== undefined
      ? {
          market_rows: market.subscriptions.reduce(
            (top, row) => Math.max(top, row.revision ?? 0),
            0,
          ),
          market_set: market.subscription_set,
          catalog: market.catalog.revision,
        }
      : {};
  const result = (await call("runtime.snapshot", {
    since,
    ...cursor,
  })) as Partial<SnapshotUnchanged>;
  if (result?.unchanged === true) {
    if (typeof result.revision !== "number" || typeof result.refreshed_at_ms !== "number")
      throw new Error(t("本机核心返回了不支持的状态契约"));
    return result as SnapshotUnchanged;
  }
  return mergeMarket(snapshotContract(result), market ?? null);
}
function mergeMarket(next: Snapshot, held: LiveMarket | null): Snapshot {
  const market = next.market;
  if (!market || (!market.delta && !market.catalog.omitted)) return next;
  if (!held) throw new Error(t("本机核心返回了不支持的状态契约"));
  const catalog = market.catalog.omitted
    ? { ...market.catalog, contracts: held.catalog.contracts, omitted: undefined }
    : market.catalog;
  let subscriptions = market.subscriptions;
  if (market.delta) {
    // The subscription set is unchanged, so held order is authoritative.
    const changed = new Map(market.subscriptions.map(row => [`${row.venue}.${row.symbol}`, row]));
    subscriptions = held.subscriptions.map(row => changed.get(`${row.venue}.${row.symbol}`) ?? row);
  }
  return { ...next, market: { ...market, catalog, subscriptions, delta: undefined } };
}
