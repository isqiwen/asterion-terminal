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
