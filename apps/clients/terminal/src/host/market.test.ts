import { create, type MessageInitShape } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import { CatalogState, QuoteFrameSchema, Subscription } from "../generated/asterion/market_pb";
import { applyFrames } from "./market";

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
  expect(view!.ticks.filter(tick => tick.symbol === "rb2611").map(tick => tick.price)).toEqual([
    "3510",
    "3511",
  ]);
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
  expect(after.view!.ticks).toHaveLength(1);
  expect(after.view!.interrupted).toBe(true);
});
