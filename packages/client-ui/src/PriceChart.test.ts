import { expect, test } from "vitest";
import { chartRuns } from "./PriceChart";

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
