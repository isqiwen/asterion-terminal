import type { Candle } from "@asterion/client-ui/PriceChart";

// Presentation indicators only. Task and execution do not consume these values.
export const averagePeriods = [5, 10, 20, 30, 60] as const;
export const historyLookback = Math.max(...averagePeriods) - 1;
export function movingAverage(
  bars: readonly Pick<Candle, "close">[],
  period: number,
): (number | null)[] {
  if (!Number.isInteger(period) || period <= 0) throw new Error("Invalid moving-average period");
  const closes = bars.map(bar => Number(bar.close));
  return closes.map((_, index) => {
    if (index + 1 < period) return null;
    // Direct bounded windows avoid cumulative drift between paged requests.
    const window = closes.slice(index + 1 - period, index + 1);
    return window.every(Number.isFinite)
      ? window.reduce((sum, value) => sum + value, 0) / period
      : null;
  });
}
