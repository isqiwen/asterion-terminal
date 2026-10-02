import { getLocale, translate } from "../../src/i18n";
import type {
  HistoryContractCatalog,
  NativeHistorySource,
  Snapshot,
  ResearchResult,
  ResearchTask,
  TerminalCommand,
} from "../../src/bridge/client";

// Data-workbench extension contract. Provider rules stay beside their adapter;
// the host and shared page do not infer capabilities from provider names.
export type HistorySource = {
  id: string;
  name: string;
  asset: string;
  dataType: string;
  description: string;
  catalogCommand: TerminalCommand;
  exchanges: readonly string[];
  catalog: (snapshot: Snapshot | null) => HistoryContractCatalog | null;
  credential: { label: string; required: boolean; maxLength: number; help: string };
  intervals: readonly (number | "day")[];
  timeAxis: "instant" | "trading-day";
  timezone: string;
  rate: { default: number; max: number };
  command: TerminalCommand;
  parameters: (
    query: HistoryQuery,
    credential: string,
    catalog: HistoryContractCatalog,
  ) => Record<string, unknown>;
  ownsTask: (task: ResearchTask) => boolean;
  dataset: (result: ResearchResult) => HistoryDataset | null;
};
export type HistoryQuery = {
  code: string;
  frequency: string;
  exchange: string;
  product: string;
  rate: string;
};
export type HistoryDataset = {
  contract: string;
  interval: number | "day";
  begin: string;
  end: string;
  rows: number;
  directory: string;
  digest: string;
};
export function historySources(declared: readonly NativeHistorySource[]): HistorySource[] {
  return declared.map(info => {
    const daily = info.intervals.length === 1 && info.intervals[0] === 0;
    return {
      id: info.id,
      name: info.name,
      asset: "期货",
      dataType: daily ? "日 K 线" : "分钟 K 线",
      description: daily
        ? "选择具体月份合约，下载存续期内按交易日期记录的日线。需要合约资料与日线数据权限。"
        : "选择具体月份合约，自动下载整个存续期的分钟数据。需要合约资料与分钟数据权限。",
      catalogCommand: "research.contracts.load",
      exchanges: info.venues,
      intervals: daily ? ["day"] : info.intervals,
      timeAxis: daily ? "trading-day" : "instant",
      timezone: info.timezone,
      rate: {
        default:
          info.connection?.requests_per_minute_default ??
          Math.min(60, info.max_requests_per_minute),
        max: info.connection?.requests_per_minute_max ?? info.max_requests_per_minute,
      },
      credential: {
        label: info.connection
          ? getLocale() === "zh-CN"
            ? info.connection.credential_label_zh
            : info.connection.credential_label_en
          : translate("asterion.terminal.data-workbench", "数据源凭据"),
        required: info.credential_required,
        maxLength: info.connection?.credential_max_length ?? 256,
        help: "只用于这一次查询和下载，提交后清空。",
      },
      catalog: snapshot =>
        snapshot?.history_contracts?.source === info.id ? snapshot.history_contracts : null,
      command: daily ? "research.daily.submit" : "research.minutes.submit",
      parameters: (query, token, catalog) => ({
        source: info.id,
        contract_id: query.code,
        catalog_cutoff_ns: catalog.cutoff_ns,
        ...(daily ? {} : { interval_minutes: Number(query.frequency) }),
        requests_per_minute: Number(query.rate),
        token,
      }),
      ownsTask: task =>
        task.kind === (daily ? "daily_download" : "minute_download") &&
        task.data_source === info.id,
      dataset: result => {
        if (result.kind === "minute_download" && !daily)
          return {
            contract: result.experiment.contract_id,
            interval: result.experiment.interval_minutes,
            begin: result.experiment.begin_ns,
            end: result.experiment.end_ns,
            rows: result.result.rows,
            directory: result.result.directory,
            digest: result.result.manifest_sha256,
          };
        if (result.kind === "daily_download" && daily)
          return {
            contract: result.experiment.contract_id,
            interval: "day",
            begin: result.experiment.begin_day,
            end: result.experiment.end_day,
            rows: result.result.rows,
            directory: result.result.directory,
            digest: result.result.manifest_sha256,
          };
        return null;
      },
    };
  });
}
