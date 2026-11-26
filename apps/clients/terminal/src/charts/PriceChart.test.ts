import { expect, test } from "vitest";
import { chartRuns, defaultVisibleCount, minimumTickSpan, numberText } from "./price-chart-math";

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

test("minimum tick span expands quiet ranges and tolerates series without visible data", () => {
  expect(minimumTickSpan({ priceRange: { minValue: 100, maxValue: 102 } }, 1)).toEqual({
    priceRange: { minValue: 91, maxValue: 111 },
  });
  const wide = { priceRange: { minValue: 100, maxValue: 130 } };
  expect(minimumTickSpan(wide, 1)).toBe(wide);
  const empty = { priceRange: null };
  expect(minimumTickSpan(empty, 1)).toBe(empty);
  expect(minimumTickSpan(null, 1)).toBeNull();
});

test("axis numbers show zero without a floating-point residue and keep real values", () => {
  expect(numberText(5.5511151231257827e-17)).toBe("0");
  expect(numberText(-2.7755575615628914e-16)).toBe("0");
  expect(numberText(0.00000002)).toBe("2e-8");
  expect(numberText(3512.5)).toBe("3512.5");
  expect(numberText(0.123456789)).toBe("0.12345679");
});
