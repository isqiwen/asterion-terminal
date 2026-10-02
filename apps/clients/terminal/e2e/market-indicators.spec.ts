import { test, expect } from "./test";
import { chartTime } from "../plugins/futures-market/chart-time";
import { movingAverage, historyLookback } from "../plugins/futures-market/indicators";
const bars = Array.from({ length: 200 }, (_, index) => ({
  timestamp_ns: String(index),
  open: String(index + 1),
  high: String(index + 1),
  low: String(index + 1),
  close: String(index + 1),
  volume: "1",
}));
test("display averages have an explicit warmup and remain identical across pages", () => {
  const average = movingAverage(bars, 60);
  expect(average.slice(0, 59).every(value => value === null)).toBe(true);
  expect(average[59]).toBe(30.5);
  expect(average[100]).toBe(71.5);
  const secondPage = movingAverage(bars.slice(100 - historyLookback), 60).slice(historyLookback);
  expect(secondPage).toEqual(average.slice(100));
  expect(movingAverage(bars.slice(0, 5), 5)).toEqual([null, null, null, null, 3]);
  expect(() => movingAverage(bars, 0)).toThrow();
  expect(movingAverage([{ ...bars[0], close: "invalid" }], 1)).toEqual([null]);
});

test("axis labels use the exchange day across UTC midnight boundaries", () => {
  const ns = String(BigInt(Date.parse("2026-09-29T16:01:00Z")) * 1000000n);
  expect(chartTime(ns, true)).toBe("09/30 00:01");
  expect(chartTime(ns, false)).toBe("00:01");
  expect(chartTime(ns, false, true)).toBe("00:01:00");
});
