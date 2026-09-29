import type { HistorySource } from "./history-sources";
import { tushareMinutes } from "./tushare-minutes";
export const tushareDaily: HistorySource = {
  ...tushareMinutes,
  id: "tushare.fut_daily",
  dataType: "日 K 线",
  description: "选择具体月份合约，下载存续期内按交易日期记录的日线。需要合约资料与日线数据权限。",
  timeAxis: "trading-day",
  intervals: ["day"],
  command: "research.daily.submit",
  parameters: (query, token, catalog) => ({
    ts_code: query.code,
    catalog_cutoff_ns: catalog.cutoff_ns,
    requests_per_minute: Number(query.rate),
    token,
  }),
  ownsTask: task => task.kind === "daily_download",
  dataset: result =>
    result.kind === "daily_download"
      ? {
          contract: result.experiment.ts_code,
          interval: "day",
          begin: result.experiment.begin_day,
          end: result.experiment.end_day,
          rows: result.result.rows,
          directory: result.result.directory,
          digest: result.result.manifest_sha256,
        }
      : null,
};
