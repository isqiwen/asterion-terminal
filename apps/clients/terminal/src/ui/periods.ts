import { Period } from "../generated/asterion/common_pb";

// A chart's period as the pages name it: minutes, or a calendar span.
export type ChartPeriod = number | "day" | "week" | "month" | "quarter" | "year";

const periods: Record<string, Period> = {
  1: Period.MINUTES_1,
  5: Period.MINUTES_5,
  10: Period.MINUTES_10,
  15: Period.MINUTES_15,
  30: Period.MINUTES_30,
  60: Period.MINUTES_60,
  day: Period.DAY,
  week: Period.WEEK,
  month: Period.MONTH,
  quarter: Period.QUARTER,
  year: Period.YEAR,
};

export const protocolPeriod = (period: ChartPeriod) => periods[String(period)]!;
