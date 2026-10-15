import { getLocale, translate } from "../../src/i18n";
import type {
  NativeHistorySource,
  TableContracts,
  TaskResult,
  TaskRecord,
} from "../../src/bridge/client";

// Data-workbench extension contract. Provider rules stay beside their adapter;
// the host and shared page do not infer capabilities from provider names.
export type HistorySource = {
  id: string;
  name: string;
  // The data source it belongs to: its contracts are that source's contract list's.
  pluginId: string;
  asset: string;
  dataType: string;
  description: string;
  exchanges: readonly string[];
  credential: { label: string; required: boolean; maxLength: number; help: string };
  intervals: readonly number[];
  timezone: string;
  rate: { default: number; max: number };
  parameters: (
    query: HistoryQuery,
    credential: string,
    contracts: TableContracts,
  ) => Record<string, unknown>;
  ownsTask: (task: TaskRecord) => boolean;
  dataset: (result: TaskResult) => HistoryDataset | null;
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
  interval: number;
  begin: string;
  end: string;
  rows: number;
  directory: string;
  digest: string;
};
export function historySources(declared: readonly NativeHistorySource[]): HistorySource[] {
  return declared.map(info => ({
    id: info.id,
    name: info.name,
    pluginId: info.plugin_id,
    asset: "期货",
    dataType: "分钟 K 线",
    description:
      "选择具体月份合约，自动下载整个存续期的分钟数据。合约取自这个数据源的合约表，需要分钟数据权限。",
    exchanges: info.venues,
    intervals: info.intervals,
    timezone: info.timezone,
    rate: {
      default:
        info.connection?.requests_per_minute_default ?? Math.min(60, info.max_requests_per_minute),
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
    parameters: (query, token, contracts) => ({
      source: info.id,
      contract_id: query.code,
      snapshot: contracts.snapshot,
      cutoff_ns: contracts.cutoff_ns,
      interval_minutes: Number(query.frequency),
      requests_per_minute: Number(query.rate),
      token,
    }),
    ownsTask: task => task.kind === "minute_download" && task.data_source === info.id,
    dataset: result =>
      result.kind === "minute_download"
        ? {
            contract: result.experiment.contract_id,
            interval: result.experiment.interval_minutes,
            begin: result.experiment.begin_ns,
            end: result.experiment.end_ns,
            rows: result.result.rows,
            directory: result.result.directory,
            digest: result.result.manifest_sha256,
          }
        : null,
  }));
}
