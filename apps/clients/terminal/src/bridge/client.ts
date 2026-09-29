import { BackendError } from "../i18n/errors";
import { translate, type MessageValues, getLocale } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { desktop, save } from "./desktop";
import { nativeDesktop } from "./desktop";
export type Tick = {
  timestamp_ns: string;
  price: string;
  quantity: string;
};
export type Dataset = {
  filename: string;
  venue: string;
  symbol: string;
  product: string;
  delivery_month: string;
  currency: string;
  price_increment: string;
  quantity_increment: string;
  multiplier: string;
  count: number;
  quantity: string;
  first_timestamp_ns: string | null;
  last_timestamp_ns: string | null;
  last_price: string | null;
  ticks: Tick[];
  source: "local_csv" | "published_csv";
  publication_ready: boolean;
  publication_id?: string;
  revision?: string;
  specification_source: "user_supplied";
  persistent: boolean;
};
export type PaperAccount = {
  replay?: {
    publication: CalendarPublication;
    settled_days: number;
    settlement_due: boolean;
    session_end: boolean;
  };
  mode: "historical_paper";
  persistent: true;
  storage_state: "ready" | "recovery_required";
  strategy?: {
    grant_id: string;
    strategy_id: string;
    stream_id: string;
    dataset_revision: string;
    max_quantity: string;
    active: boolean;
    last_sequence: number;
  };
  balance: string;
  equity: string;
  available: string;
  margin: string;
  frozen: string;
  fees: string;
  realized: string;
  unrealized: string;
  mark: string;
  cursor: number;
  total: number;
  timestamp_ns: string | null;
  contract: {
    venue: string;
    symbol: string;
    currency: string;
    product: string;
    delivery_month: string;
    price_increment: string;
    quantity_increment: string;
    multiplier: string;
  };
  risk: { max_order_quantity: string; max_gross_quantity: string; max_working_orders: number };
  costs: {
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
  positions: {
    side: "buy" | "sell";
    bucket: "today" | "yesterday";
    quantity: string;
    basis: string;
  }[];
  orders: {
    id: string;
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
    quantity: string;
    price: string;
  }[];
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
  | "node.update"
  | "strategy.run"
  | "strategy.attach"
  | "strategy.revoke"
  | "research.local"
  | "research.attach"
  | "research.submit"
  | "research.factor.submit"
  | "research.daily-factor.submit"
  | "research.calendar.submit"
  | "research.data.submit"
  | "research.daily.page"
  | "research.daily.submit"
  | "research.minutes.page"
  | "research.minutes.submit"
  | "research.contracts.load"
  | "research.data.use"
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
  | "paper.create"
  | "paper.open"
  | "paper.close"
  | "paper.act"
  | "paper.connect"
  | "paper.reconnect"
  | "node.bootstrap"
  | "node.connect"
  | "node.local"
  | "node.disconnect"
  | "node.deploy"
  | "node.action"
  | "node.attach";
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
      kind: "paper" | "market" | "research" | "strategy";
      active_workers: number;
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
export type DatasetPublication = {
  version: number;
  id: string;
  source_name: string;
  source_sha256: string;
  source_bytes: number;
  importer: string;
  dataset: {
    version: number;
    revision: string;
    ticks: Tick[];
    contract: Pick<
      Dataset,
      | "venue"
      | "symbol"
      | "product"
      | "delivery_month"
      | "currency"
      | "price_increment"
      | "quantity_increment"
      | "multiplier"
    >;
  };
};
export type ResearchTask = {
  kind:
    | "backtest"
    | "factor"
    | "daily_factor"
    | "data_import"
    | "calendar_import"
    | "minute_download"
    | "daily_download";
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
  account: Omit<PaperAccount, "mode" | "persistent"> & { mode: "backtest"; persistent: false };
  equity: { timestamp_ns: string; equity: string; event: "trade" | "settlement" }[];
  settlements: {
    trading_day: string;
    timestamp_ns: string;
    price: string;
    balance: string;
    equity: string;
    realized: string;
    fees: string;
    position_quantity: string;
  }[];
};
export type ExperimentData = {
  count: number;
  first_timestamp_ns: string;
  last_timestamp_ns: string;
};
export type ExperimentContract = DatasetPublication["dataset"]["contract"];
export type BacktestExperiment = {
  version: number;
  calendar_publication: CalendarPublication | null;
  dataset_revision: string;
  days: {
    trading_day: string;
    sessions: { begin_ns: string; end_ns: string }[];
    schedule_source: string;
    settlement_price: string;
    settlement_source: string;
  }[];
  sma: { fast: number; slow: number; quantity: string };
  paper: {
    version: number;
    type: "historical_paper";
    contract: ExperimentContract;
    deposit: string;
    costs: PaperAccount["costs"];
    risk: PaperAccount["risk"];
  };
  data: ExperimentData;
};
export type FactorExperiment = {
  version: number;
  dataset_revision: string;
  contract: ExperimentContract;
  lookbacks: number[];
  horizon: number;
  evaluation:
    | { mode: "full_sample" }
    | { mode: "holdout"; split_index: number }
    | { mode: "walk_forward"; training_events: number; validation_events: number };
  data: ExperimentData;
};
export type CalendarPublication = Omit<DatasetPublication, "dataset"> & {
  calendar: {
    version: number;
    revision: string;
    contract: ExperimentContract;
    days: BacktestExperiment["days"];
  };
};
export type DailyFactorExperiment = {
  version: number;
  dataset_revision: string;
  lookback: number;
  horizon: number;
  evaluation: { mode: "full_sample" } | { mode: "holdout"; split_index: number };
  data: {
    source_task_id: string;
    source: string;
    ts_code: string;
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
  | { id: string; kind: "data_import"; task: ResearchTask; result: DatasetPublication }
  | { id: string; kind: "calendar_import"; task: ResearchTask; result: CalendarPublication }
  | {
      id: string;
      kind: "minute_download";
      task: ResearchTask;
      experiment: { ts_code: string; interval_minutes: number; begin_ns: string; end_ns: string };
      result: { directory: string; manifest_sha256: string; rows: number; pages: number };
    }
  | {
      id: string;
      kind: "daily_download";
      task: ResearchTask;
      experiment: { ts_code: string; begin_day: string; end_day: string };
      result: { directory: string; manifest_sha256: string; rows: number; pages: number };
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
  ts_code: string;
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
export type Snapshot = {
  daily_page: DailyPage | null;
  history_page: HistoryPage | null;
  history_contracts: HistoryContractCatalog;
  // Present when the core answered from its last snapshot because another operation was running.
  stale?: true;
  // Revision of the core's published state and when the core last refreshed it.
  revision: number;
  refreshed_at_ms: number;
  strategy?: null | {
    id: string;
    state: "connected" | "disconnected";
    phase: string;
    processed: number;
    total: number;
    fast?: number;
    slow?: number;
    quantity?: string;
    symbol?: string;
    error: string;
    account?: string;
    grant_id?: string;
    revision?: string;
  };
  research: null | {
    connection_id: string;
    service: string;
    host: string;
    remote: boolean;
    online: boolean;
    error: string;
    tasks: ResearchTask[];
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
  connection: {
    transport: "local" | "tcp_tls";
    state: "connected" | "disconnected";
    host?: string;
    port?: number;
    session?: string;
    mode?: "paper";
    restarts: number;
    reconnects: number;
    last_heartbeat_ms: number;
    latency_ms: number;
    health: null | {
      instance_id: string;
      version: string;
      uptime_ms: number;
      phase: string;
    };
  } | null;
  dataset: Dataset | null;
  plugins: {
    id: string;
    kind: string;
    state: string;
  }[];
  diagnostics: { succeeded: number; failed: number; trading_process_id: number | null };
  live_market: "not_connected";
  execution: "paper_only";
  paper: PaperAccount | null;
};
export type CsvRequest = {
  path: string;
  venue: string;
  symbol: string;
  product: string;
  delivery_month: string;
  currency: string;
  price_increment: string;
  quantity_increment: string;
  multiplier: string;
};
async function call(
  method: "runtime.snapshot" | "futures.inspect_csv" | TerminalCommand,
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
  method: "runtime.snapshot" | "futures.inspect_csv" | TerminalCommand,
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
