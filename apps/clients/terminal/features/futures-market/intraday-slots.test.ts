import { create } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import { MinutesSchema } from "../../src/generated/asterion/market_pb";
import { intradaySlots } from "./intraday-slots";

test("the average price shows to the price's decimals, at least two", () => {
  const bar = (close: string) => ({
    startMs: 1_762_783_200_000n,
    open: { text: close },
    high: { text: close },
    low: { text: close },
    close: { text: close },
    averagePrice: { text: "15557.64026541" },
    volume: 1n,
  });
  const average = (close: string) =>
    intradaySlots(create(MinutesSchema, { bars: [bar(close)] }), null).slots[0]!.average;
  expect(average("15625")).toBe("15557.64");
  expect(average("106.920")).toBe("15557.640");
});

test("open interest carries across quiet minutes inside the observed span", () => {
  const base = Date.UTC(2026, 9, 13, 1, 1);
  const series = create(MinutesSchema, {
    bars: [
      {
        startMs: BigInt(base),
        open: { text: "3500" },
        high: { text: "3500" },
        low: { text: "3500" },
        close: { text: "3500" },
        volume: 10n,
        openInterest: { text: "120000" },
      },
      {
        startMs: BigInt(base + 120_000),
        open: { text: "3502" },
        high: { text: "3502" },
        low: { text: "3502" },
        close: { text: "3502" },
        volume: 5n,
        openInterest: { text: "120050" },
      },
    ],
  });
  const { slots } = intradaySlots(series, [["09:00", "09:05"]]);
  expect(slots.map(slot => slot.openInterest)).toEqual([null, "120000", "120000", "120050", null]);
});
