import { create, type MessageInitShape } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import { CatalogState, QuoteFrameSchema, Subscription } from "../generated/asterion/market_pb";
import { applyFrames, contractGeneration, isMarketSessionOpen } from "./market";

const row = (symbol: string, last: string, exchangeMs: bigint) => ({
  instrument: { exchange: "SHFE", symbol },
  subscription: Subscription.ACTIVE,
  quote: { last: { text: last }, exchangeMs, tradingDay: "2026-10-13" },
});
const frame = (value: MessageInitShape<typeof QuoteFrameSchema>["frame"]) =>
  create(QuoteFrameSchema, { frame: value });
const full = frame({
  case: "full",
  value: {
    rows: [row("rb2610", "3500", 1000n), row("rb2611", "3510", 1000n)],
    catalog: {
      status: { state: CatalogState.READY },
      contracts: [{ instrument: { exchange: "SHFE", symbol: "rb2610" } }],
    },
  },
});

test("changed frames replace whole rows and keep the catalog they leave out", () => {
  const changed = frame({ case: "changed", value: { rows: [row("rb2611", "3511", 2000n)] } });
  const { view, restart } = applyFrames(null, [full, changed], true);
  expect(restart).toBe(false);
  expect(view!.board.rows.map(item => item.quote?.last?.text)).toEqual(["3500", "3511"]);
  expect(view!.board.catalog?.contracts).toHaveLength(1);
  expect(view!.ticks.get("SHFE.rb2611")!.map(tick => tick.price)).toEqual(["3510", "3511"]);
  const cleared = frame({
    case: "changed",
    value: {
      rows: [
        { instrument: { exchange: "SHFE", symbol: "rb2611" }, subscription: Subscription.PENDING },
      ],
      catalog: { status: { state: CatalogState.LOADING } },
    },
  });
  const after = applyFrames(view, [cleared], true);
  expect(after.view!.board.rows[1]!.quote).toBeUndefined();
  expect(after.view!.board.catalog?.contracts).toHaveLength(0);
});

test("a changed frame before any full frame reopens the stream", () => {
  const changed = frame({ case: "changed", value: {} });
  expect(applyFrames(null, [changed], true)).toEqual({ view: null, restart: true });
});

test("a new full frame replaces membership and resets the tick curve", () => {
  const { view } = applyFrames(null, [full], true);
  const reset = frame({
    case: "full",
    value: {
      rows: [row("au2612", "900", 2000n)],
      catalog: { status: { state: CatalogState.READY } },
    },
  });
  const after = applyFrames(view, [reset], true);
  expect(after.restart).toBe(false);
  expect(after.view!.board.rows.map(item => item.instrument!.symbol)).toEqual(["au2612"]);
  expect(after.view!.ticks.size).toBe(1);
  expect(after.view!.interrupted).toBe(true);
});

test("a busy contract does not push out another contract's recent quotes", () => {
  const busy = Array.from({ length: 600 }, (_, i) =>
    frame({ case: "changed", value: { rows: [row("rb2611", "3511", BigInt(2000 + i))] } }),
  );
  const { view } = applyFrames(null, [full, ...busy], true);
  expect(view!.ticks.get("SHFE.rb2610")!.map(tick => tick.price)).toEqual(["3500"]);
  expect(view!.ticks.get("SHFE.rb2611")).toHaveLength(512);
});

test("empty polls keep the same MarketView and ticks references when online status is unchanged", () => {
  const { view } = applyFrames(null, [full], true);
  const idle = applyFrames(view, [], true);
  expect(idle.restart).toBe(false);
  expect(idle.view).toBe(view);
  expect(idle.view!.ticks).toBe(view!.ticks);
});

test("a day or volume reset on one contract does not bump another contract's generation", () => {
  const initial = frame({
    case: "full",
    value: {
      rows: [
        {
          instrument: { exchange: "SHFE", symbol: "rb2610" },
          subscription: Subscription.ACTIVE,
          quote: {
            last: { text: "3500" },
            exchangeMs: 1000n,
            tradingDay: "2026-10-13",
            volume: 10n,
          },
        },
        {
          instrument: { exchange: "SHFE", symbol: "rb2611" },
          subscription: Subscription.ACTIVE,
          quote: {
            last: { text: "3510" },
            exchangeMs: 1000n,
            tradingDay: "2026-10-13",
            volume: 20n,
          },
        },
      ],
    },
  });
  const before = applyFrames(null, [initial], true).view!;
  const beforeRb10 = contractGeneration(before, "SHFE.rb2610");
  const beforeRb11 = contractGeneration(before, "SHFE.rb2611");
  const roll = frame({
    case: "changed",
    value: {
      rows: [
        {
          instrument: { exchange: "SHFE", symbol: "rb2611" },
          subscription: Subscription.ACTIVE,
          quote: {
            last: { text: "3512" },
            exchangeMs: 2000n,
            tradingDay: "2026-10-14",
            volume: 1n,
          },
        },
      ],
    },
  });
  const after = applyFrames(before, [roll], true).view!;
  expect(contractGeneration(after, "SHFE.rb2610")).toBe(beforeRb10);
  expect(contractGeneration(after, "SHFE.rb2611")).not.toBe(beforeRb11);
});

test("isMarketSessionOpen matches Beijing day, night and Saturday early-morning trading windows", () => {
  const bj = (iso: string) => new Date(`${iso}+08:00`);
  // Weekday day & night windows (with 15m pre-open / post-close buffer).
  expect(isMarketSessionOpen(bj("2026-10-12T08:44:00"))).toBe(false);
  expect(isMarketSessionOpen(bj("2026-10-12T08:45:00"))).toBe(true);
  expect(isMarketSessionOpen(bj("2026-10-13T09:32:00"))).toBe(true);
  expect(isMarketSessionOpen(bj("2026-10-13T15:30:00"))).toBe(true);
  expect(isMarketSessionOpen(bj("2026-10-13T15:31:00"))).toBe(false);
  expect(isMarketSessionOpen(bj("2026-10-13T20:45:00"))).toBe(true);
  // Friday night runs into Saturday 02:30 (+15m buffer); Sunday & Monday early morning are closed.
  expect(isMarketSessionOpen(bj("2026-10-17T02:30:00"))).toBe(true);
  expect(isMarketSessionOpen(bj("2026-10-17T02:46:00"))).toBe(false);
  expect(isMarketSessionOpen(bj("2026-10-18T10:00:00"))).toBe(false);
  expect(isMarketSessionOpen(bj("2026-10-12T02:00:00"))).toBe(false);
});
