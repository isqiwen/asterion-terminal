import type { DailyBar, HistoryBar } from "../../bridge/client";
// Chart coordinates only: UTC midnight gives the shared renderer an ordered axis.
// Provider trading_day remains authoritative; this value never enters a data query or order.
export function dailyChartBar(bar: DailyBar): HistoryBar & DailyBar {
  return {
    ...bar,
    timestamp_ns: (BigInt(Date.parse(`${bar.trading_day}T00:00:00Z`)) * 1000000n).toString(),
  };
}
export function dailyChartDate(coordinate: string): string {
  return new Date(Number(BigInt(coordinate) / 1000000n)).toISOString().slice(0, 10);
}
