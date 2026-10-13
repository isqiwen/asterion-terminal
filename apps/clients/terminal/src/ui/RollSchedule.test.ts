import { describe, expect, it } from "vitest";
import { daysWithoutBars } from "./RollSchedule";

describe("the days of a dominant series its dominant month has no bars on", () => {
  // rb2610 is dominant from the 2nd and rb2701 from the 6th. rb2610 is read
  // through the 7th to be closed, and rb2701 from the 5th to take over.
  const rolls = [
    { trading_day: "2026-06-02", symbol: "rb2610" },
    { trading_day: "2026-06-06", symbol: "rb2701" },
  ];

  it("counts a month while it is dominant and not beside that", () => {
    const months = [
      // Halted on the 4th; no bar on the roll day either, when it is no longer dominant.
      { symbol: "rb2610", uncovered_days: ["2026-06-04", "2026-06-06"] },
      // No bar on the day before it takes over, and none on the 8th.
      { symbol: "rb2701", uncovered_days: ["2026-06-05", "2026-06-08"] },
    ];
    expect(daysWithoutBars(rolls, months)).toEqual(["2026-06-04", "2026-06-08"]);
  });

  it("is empty where every dominant day has bars", () => {
    const months = [
      { symbol: "rb2610", uncovered_days: [] },
      { symbol: "rb2701", uncovered_days: ["2026-06-05"] },
    ];
    expect(daysWithoutBars(rolls, months)).toEqual([]);
  });
});
