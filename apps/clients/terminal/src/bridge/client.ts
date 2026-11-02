import { BackendError } from "../i18n/errors";
import { translate, type MessageValues, getLocale } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { desktop } from "./desktop";
import type { JsonValue } from "@bufbuild/protobuf";

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
export type { TerminalCommand, CommandParams, CommandCall, CommandHandler } from "./commands";
import type { TerminalCommand, CommandCall } from "./commands";
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
    pid: number;
    os: string;
    arch: string;
    version: string;
    uptime_ms: number;
    services: {
      kind: "lab";
      id: string;
      artifact: string;
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
export type FactorMembershipReason =
  | "MEMBERSHIP_ELIGIBLE"
  | "MEMBERSHIP_UNOBSERVED"
  | "MEMBERSHIP_INSUFFICIENT_HISTORY"
  | "MEMBERSHIP_UNKNOWN_VOLUME"
  | "MEMBERSHIP_UNKNOWN_OPEN_INTEREST"
  | "MEMBERSHIP_BELOW_VOLUME"
  | "MEMBERSHIP_BELOW_OPEN_INTEREST";
export type FactorObservation = {
  feature_state?:
    "FEATURE_VALUE" | "FEATURE_WARMUP" | "FEATURE_ZERO_DENOMINATOR" | "FEATURE_NON_FINITE";
  role: "full_sample" | "development" | "holdout";
  label_crosses_boundary: boolean;
  event_index: number;
  observed: string;
  value: number | null;
  forward_return: number | null;
  label_end_index: number | null;
  label: string | null;
  evaluation_warmup: boolean;
  included: boolean;
};
export type FactorResult = {
  observations?: FactorObservation[]; // Present only when explicitly retained in this run.
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
    observations?: FactorObservation[];
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
export type UnreadableCtpConnection = { id: string; name: string; error: "unreadable" };
export type TradingConnection = {
  id: string;
  name: string;
  host: string;
  port: number;
  instance: string;
  fingerprint: string;
  revision: string;
};

export type TablePartitionKey = { table: string; exchange: string; period: string };
// What an update of market-wide tables was asked to read. An empty begin day
// continues after what each exchange already holds; an empty end day reads
// through the last day that has ended.
export type TableRevision = TablePage["revision"];
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
// The Python runtime the first launch installs: each module's bytes so far.
export type PythonRuntime = {
  state: "missing" | "installing" | "ready" | "failed";
  error: string;
  modules: {
    id: string;
    title: string;
    size: number;
    done: number;
    state: "waiting" | "downloading" | "downloaded" | "installing" | "done" | "failed";
  }[];
};
export type Snapshot = {
  market_data?: JsonValue;
  trading_connections?: (TradingConnection | UnreadableCtpConnection)[];
  trading_fingerprint?: string;
  // The saved Market host, or a mark that its settings file is unreadable.
  market_endpoint?:
    | null
    | {
        host: string;
        port: number;
        fingerprint: string;
        instruments: { venue: string; symbol: string }[];
      }
    | { error: "unreadable" };
  strategy_repository?: null | { path: string } | { error: "unreadable" };
  python_runtime?: PythonRuntime;
  // Revision of the core's published state and when the core last refreshed it.
  revision: number;
  refreshed_at_ms: number;
  market: LiveMarket | null;
  intraday?: IntradaySeries;
  // ProtoJSON, decoded with the generated recording.Status schema.
  recording?: Record<string, string | number | boolean>;
  nodes: NodeStatus[];
  protocol: 1;
  product: string;
  core: string;
  phase: "ready";
  asset: "futures";
  // The core's rule per exchange for assigning closes to today's and
  // yesterday's positions.
  close_policies: Record<string, "explicit_buckets" | "today_first" | "yesterday_first">;
  diagnostics: {
    log_directory: string;
    succeeded: number;
    failed: number;
    log_failures: number;
    refresh_failures: number;
    refresh_failed: boolean;
  };
};
// One native command; its reply as the core sent it.
export async function call(
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
  if (!value || value.protocol !== 1 || value.phase !== "ready" || value.asset !== "futures") {
    throw new Error(t("本机核心返回了不支持的状态契约"));
  }
  return value;
}
export async function request(
  ...args: CommandCall<keyof import("./commands").CommandParams>
): Promise<Snapshot> {
  return snapshotContract(await call(args[0], args[1] ?? {}));
}
export function timestamp(ns: string | null): string {
  if (!ns) return "—";
  const date = new Date(Number(BigInt(ns) / 1000000n));
  return date.toLocaleString(getLocale(), { hour12: false, timeZone: "Asia/Shanghai" });
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
