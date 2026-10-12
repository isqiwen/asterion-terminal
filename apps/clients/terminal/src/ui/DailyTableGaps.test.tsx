import { render } from "@testing-library/react";
import { expect, test } from "vitest";
import type { TableCoverageRow } from "../bridge/client";
import { DailyTableGaps } from "./DailyTableGaps";

const stored = (missing_days: number | null): TableCoverageRow => ({
  table: "futures.daily",
  schema: 1,
  source: "tushare.fut_daily.v3",
  exchange: "SHFE",
  partitions: 2617,
  rows: 494345,
  first_period: "1996-06-17",
  last_period: "2026-10-09",
  acquired_at_ns: "1791717000000000000",
  missing_days,
  missing_examples: [],
});

test("what the stored range lacks is said beside its first and last day", () => {
  // Whole: nothing to add.
  expect(render(<DailyTableGaps stored={stored(0)} />).container.textContent).toBe("");
  expect(render(<DailyTableGaps stored={stored(451)} />).container.textContent).toBe(
    "这个范围内有 451 个交易日还没有入库；数据集用到这些交易日时会被拒绝并列出日期。",
  );
  // Without the calendar of every year of the range, the store cannot count.
  expect(render(<DailyTableGaps stored={stored(null)} />).container.textContent).toBe(
    "这个范围内的交易日历没有全部入库，中间是否缺交易日无法判断；数据集用到没有入库的交易日时会被拒绝并列出日期。",
  );
});
