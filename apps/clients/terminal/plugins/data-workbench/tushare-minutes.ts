import type { HistorySource } from "./history-sources";
export const tushareMinutes: HistorySource = {
  id: "tushare.ft_mins",
  name: "Tushare",
  asset: "期货",
  dataType: "分钟 K 线",
  description: "选择具体月份合约，自动下载整个存续期的分钟数据。需要合约资料与分钟数据权限。",
  catalogCommand: "research.contracts.load",
  exchanges: ["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"],
  catalog: snapshot =>
    snapshot?.history_contracts?.source === "tushare.ft_mins" ? snapshot.history_contracts : null,
  credential: {
    label: "Tushare Token",
    required: true,
    maxLength: 256,
    help: "Token 用于合约查询与本机下载；任务提交后清空输入。任务凭据由当前账户保护，关闭桌面不停止下载。",
  },
  timeAxis: "instant",
  intervals: [1, 5, 15, 30, 60],
  timezone: "Asia/Shanghai",
  rate: { default: 60, max: 500 },
  command: "research.minutes.submit",
  parameters: (query, token, catalog) => ({
    ts_code: query.code,
    catalog_cutoff_ns: catalog.cutoff_ns,
    interval_minutes: Number(query.frequency),
    requests_per_minute: Number(query.rate),
    token,
  }),
  // This task kind currently belongs exclusively to the Tushare adapter.
  // A future shared backend task must carry explicit source identity.
  ownsTask: task => task.kind === "minute_download",
  dataset: result =>
    result.kind === "minute_download"
      ? {
          contract: result.experiment.ts_code,
          interval: result.experiment.interval_minutes,
          begin: result.experiment.begin_ns,
          end: result.experiment.end_ns,
          rows: result.result.rows,
          directory: result.result.directory,
          digest: result.result.manifest_sha256,
        }
      : null,
};
