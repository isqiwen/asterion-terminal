import { expect, test } from "vitest";
import { chartRuns, defaultVisibleCount } from "./PriceChart";

test("gaps remain separate line segments and retain their observation positions", () => {
  expect(chartRuns([null, 10, 12, null, 15, null])).toEqual([
    [
      { time: 2, value: 10 },
      { time: 3, value: 12 },
    ],
    [{ time: 5, value: 15 }],
  ]);
  expect(chartRuns([null, null])).toEqual([]);
});

test("candlestick charts size their initial window to container width while line charts show all points", () => {
  expect(defaultVisibleCount("line", 1200, 360)).toBe(1200);
  expect(defaultVisibleCount("candles", 1200, 360)).toBe(43);
  expect(defaultVisibleCount("candles", 1200, 960)).toBe(129);
  expect(defaultVisibleCount("candles", 18, 360)).toBe(18);
});
