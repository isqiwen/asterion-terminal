import { BackendError } from "../i18n/errors";
import { translate, type MessageValues, getLocale } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { desktop, save } from "./desktop";
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
// Bars resolved by the research service from completed data-source downloads.
export type DatasetSelection = {
  source_dataset_ids: string[];
  settlement_dataset_ids: string[];
  begin_day: string;
  end_day: string;
  contract: FuturesContract;
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
  // Exchange trading days (from the daily download) without minute bars.
  uncovered_days: string[];
};
// A dataset recorded in experiment evidence; bars and days are omitted.
export type DatasetEvidence = {
  version: number;
  revision: string;
  source: string;
  source_dataset_ids: string[];
  settlement_dataset_ids: string[];
  interval_minutes: number;
  contract: FuturesContract;
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
  }[];
};
// A live CTP session; the broker reports the account, the session owns the
// execution chain (authorization, allowed contracts, risk, order records).
export type LiveSession = {
  mode: "live";
  broker: { front: string; broker_id: string; user_id: string; app_id: string };
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
  // Valid for the current connection and trading day.
  authorization: null | { trading_day: string; authorized_at_ms: number };
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
  | "research.credentials.save"
  | "research.credentials.clear"
  | "research.credentials.verify"
  | "ctp.connections.save"
  | "ctp.connections.remove"
  | "ctp.connections.market"
  | "research.local.create"
  | "native.plugins.inspect"
  | "native.plugins.preview"
  | "native.plugins.install"
  | "native.plugins.uninstall"
  | "node.plugins.configure"
  | "node.update"
  | "research.local"
  | "research.attach"
  | "research.submit"
  | "research.factor.submit"
  | "research.daily-factor.submit"
  | "research.daily.page"
  | "research.daily.submit"
  | "research.minutes.page"
  | "research.minutes.submit"
  | "research.contracts.load"
  | "research.datasets"
  | "research.dataset.saved"
  | "research.dataset.save"
  | "research.dataset.use"
  | "research.coverage"
  | "research.history.usage"
  | "research.history.plan"
  | "research.history.submit"
  | "research.dataset.select"
  | "research.dataset.remove"
  | "research.action"
  | "research.result"
  | "market.local"
  | "market.attach"
  | "market.connect"
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
  | "live.open"
  | "live.connect"
  | "live.disconnect"
  | "live.costs"
  | "live.act"
  | "live.close"
  | "node.bootstrap"
  | "node.connect"
  | "node.local"
  | "node.disconnect"
  | "node.deploy"
  | "node.action";
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
    maintenance: boolean;
    pid: number;
    os: string;
    arch: string;
    version: string;
    uptime_ms: number;
    services: {
      revision: string;
      kind: "live" | "market" | "research";
      active_workers: number;
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
export type FactorResult = {
  version: number;
  dataset_revision: string;
  engine_version: string;
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
    pearson: number | null;
    spearman: number | null;
  }[];
  samples: {
    event_index: number;
    timestamp_ns: string;
    label_timestamp_ns: string;
    value: number;
    forward_return: number;
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
  const provider = snapshot?.research?.sources.find(item => item.id === source)?.plugin_id;
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
export type ResearchTask = {
  provider_artifact?: string;
  risk_artifact?: string;
  data_source?: string;
  history_dataset_id?: string;
  kind: "backtest" | "factor" | "daily_factor" | "minute_download" | "daily_download";
  id: string;
  state:
    | "queued"
    | "running"
    | "cancel_requested"
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
export type BacktestResult = {
  version: number;
  dataset_revision: string;
  engine_version: string;
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
    contracts: { venue: string; symbol: string; price: string; position_quantity: string }[];
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
  version: number;
  dataset_revision: string;
  sma: { fast: number; slow: number; quantity: string };
  paper: {
    version: number;
    type: "historical_paper";
    deposit: string;
    risk: RiskLimits;
    contracts: { dataset: DatasetEvidence; cost_schedule: CostVersion[] }[];
  };
  // One range per contract, in contract order.
  data: ExperimentData[];
};
export type FactorExperiment = {
  version: number;
  dataset_revision: string;
  dataset: DatasetEvidence;
  lookbacks: number[];
  horizon: number;
  evaluation:
    | { mode: "full_sample" }
    | { mode: "holdout"; split_index: number }
    | { mode: "walk_forward"; training_events: number; validation_events: number };
  data: ExperimentData;
};
export type DailyFactorExperiment = {
  version: number;
  dataset_revision: string;
  lookback: number;
  horizon: number;
  evaluation: { mode: "full_sample" } | { mode: "holdout"; split_index: number };
  data: {
    source_dataset_id: string;
    source: string;
    contract_id: string;
    manifest_sha256: string;
    count: number;
    first_day: string;
    last_day: string;
  };
};
export type DailyFactorResult = {
  version: number;
  engine_version: string;
  dataset_revision: string;
  input_count: number;
  purged_count: number;
  samples: {
    observation_index: number;
    trading_day: string;
    label_day: string;
    value: number;
    forward_return: number;
  }[];
  partitions: {
    name: "full_sample" | "development" | "holdout";
    begin_index: number;
    end_index: number;
    sample_count: number;
    pearson: number | null;
    spearman: number | null;
  }[];
};
export type ResearchResult =
  | {
      id: string;
      kind: "daily_factor";
      task: ResearchTask;
      experiment: DailyFactorExperiment;
      result: DailyFactorResult;
    }
  | {
      id: string;
      kind: "backtest";
      task: ResearchTask;
      experiment: BacktestExperiment;
      result: BacktestResult;
    }
  | {
      id: string;
      kind: "factor";
      task: ResearchTask;
      experiment: FactorExperiment;
      result: FactorResult;
    }
  | {
      id: string;
      kind: "minute_download";
      task: ResearchTask;
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
      kind: "daily_download";
      task: ResearchTask;
      experiment: { contract_id: string; begin_day: string; end_day: string };
      result: { directory: string; manifest_sha256: string; rows: number; pages: number };
    };
// Trading days per contract across archived minute and daily versions.
export type HistoryCoverage = {
  minute_dataset_id: string;
  daily_dataset_id: string;
  minute_source: string;
  daily_source: string;
  interval_minutes: number;
  contract_id: string;
  minute_days: number;
  minute_first: string;
  minute_last: string;
  daily_days: number;
  daily_first: string;
  daily_last: string;
  uncovered: number;
  uncovered_days: string[];
};
export type SavedResearchDataset = {
  id: string;
  name: string;
  selections: Pick<
    DatasetSelection,
    | "source_dataset_ids"
    | "settlement_dataset_ids"
    | "begin_day"
    | "end_day"
    | "contract"
    | "revision"
  >[];
};
export type HistoryReference = {
  kind: "download" | "backtest" | "bar_factor" | "daily_factor" | "saved_dataset";
  id: string;
  name: string;
  roles: ("market" | "settlement" | "output")[];
};
export type HistoryUsage = {
  dataset_id: string;
  references: HistoryReference[];
  selected_roles: ("market" | "settlement")[];
  disconnected_nodes: { names: string[]; error?: string };
  other_research: {
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
  calendar_dataset_id: string;
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
export type HistoryContractCatalog = {
  source: string;
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
export type DailyBar = Omit<HistoryBar, "timestamp_ns"> & {
  trading_day: string;
  previous_close: string | null;
  previous_settlement: string | null;
  settlement: string | null;
};
export type DailyPage = Omit<
  HistoryPage,
  "interval_minutes" | "first_ns" | "last_ns" | "begin_ns" | "end_ns" | "bars"
> & {
  period: "day" | "week" | "month" | "quarter" | "year";
  first_day: string;
  last_day: string;
  begin_day: string;
  end_day: string;
  bars: DailyBar[];
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
  daily_page: DailyPage | null;
  history_page: HistoryPage | null;
  history_contracts: HistoryContractCatalog;
  history_datasets?: HistoryDatasetRecord[];
  saved_datasets?: SavedResearchDataset[];
  history_coverage?: HistoryCoverage[];
  history_update_plan?: HistoryUpdatePlan;
  history_usage?: HistoryUsage;
  // Present when the core answered from its last snapshot because another operation was running.
  stale?: true;
  // Revision of the core's published state and when the core last refreshed it.
  revision: number;
  refreshed_at_ms: number;
  research: null | {
    connection_id: string;
    port: number;
    service: string;
    host: string;
    remote: boolean;
    online: boolean;
    error: string;
    tasks: ResearchTask[];
    sources: NativeHistorySource[];
  };
  research_result: null | ResearchResult;
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
  // Portfolio contracts selected for research, in order.
  datasets: DatasetSelection[];
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
  diagnostics: { succeeded: number; failed: number };
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
