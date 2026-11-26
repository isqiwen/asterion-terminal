import { create } from "@bufbuild/protobuf";
import { describe, expect, it } from "vitest";
import { DayCloseSchema, SessionSchema } from "../../src/generated/asterion/trading_pb";
import { buildPerformanceSeries } from "./AccountPerformance";

describe("buildPerformanceSeries", () => {
  it("computes cash-flow-adjusted NAV, drawdown, and P&L across closed days and live session", () => {
    const closes = [
      create(DayCloseSchema, {
        tradingDay: "2026-01-05",
        funds: {
          preBalance: { text: "100000" },
          dynamicEquity: { text: "105000" },
          commission: { text: "50" },
        },
      }),
      create(DayCloseSchema, {
        tradingDay: "2026-01-06",
        funds: {
          preBalance: { text: "105000" },
          deposit: { text: "50000" },
          // Base = 155,000; equity = 147,250 -> -7,750 trading loss (-5.00%), deposit excluded from return.
          dynamicEquity: { text: "147250" },
          commission: { text: "80" },
        },
      }),
    ];
    const live = create(SessionSchema, {
      tradingDay: "2026-01-07",
      funds: {
        preBalance: { text: "147250" },
        withdraw: { text: "47250" },
        // Base = 100,000; equity = 110,000 -> +10,000 trading gain (+10.00%).
        dynamicEquity: { text: "110000" },
        commission: { text: "20" },
      },
    });

    const result = buildPerformanceSeries(closes, live);
    expect(result.days).toHaveLength(3);
    expect(result.days.map(d => d.tradingDay)).toEqual(["2026-01-05", "2026-01-06", "2026-01-07"]);
    expect(result.days[2]!.isLive).toBe(true);
    expect(result.days[0]!.dayReturn).toBeCloseTo(0.05, 6);
    expect(result.days[1]!.dayReturn).toBeCloseTo(-0.05, 6);
    expect(result.days[2]!.dayReturn).toBeCloseTo(0.1, 6);
    // Compounded NAV: 1.05 * 0.95 * 1.10 = 1.09725
    expect(result.days[2]!.nav).toBeCloseTo(1.09725, 6);
    expect(result.totalReturn).toBeCloseTo(0.09725, 6);
    expect(result.cumulativePnl).toBeCloseTo(5000 - 7750 + 10000, 4);
    expect(result.maxDrawdown).toBeCloseTo(-0.05, 6);
    expect(result.winRate).toBeCloseTo(2 / 3, 6);
    expect(result.totalCommission).toBeCloseTo(150, 4);
    expect(result.netCashFlow).toBeCloseTo(2750, 4);
  });

  it("handles empty history cleanly", () => {
    const result = buildPerformanceSeries([]);
    expect(result.days).toEqual([]);
    expect(result.totalReturn).toBe(0);
    expect(result.winRate).toBeNull();
  });
});
