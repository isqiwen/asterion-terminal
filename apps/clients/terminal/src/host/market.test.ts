import { create, type MessageInitShape } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import { LiveQuotesSchema } from "../generated/asterion/market_pb";
import { applyFrames } from "./market";

const row = (symbol: string, last: string, sourceMs: bigint) => ({
  instrument: { venue: "SHFE", symbol },
  state: "subscribed",
  quote: { instrument: { venue: "SHFE", symbol }, last, sourceMs, tradingDay: "20261013" },
});
const frame = (update: MessageInitShape<typeof LiveQuotesSchema>["update"]) =>
  create(LiveQuotesSchema, { update });
const full = frame({
  case: "snapshot",
  value: {
    instanceId: "market",
    sequence: 1n,
    quotes: [row("rb2610", "3500", 1000n), row("rb2611", "3510", 1000n)],
    catalog: { phase: "ready", contracts: [{ instrument: { venue: "SHFE", symbol: "rb2610" } }] },
  },
});

test("deltas replace whole rows and preserve the omitted catalog", () => {
  const delta = frame({
    case: "delta",
    value: {
      instanceId: "market",
      sequence: 2n,
      baseSequence: 1n,
      quotes: [row("rb2611", "3511", 2000n)],
    },
  });
  const { view, restart } = applyFrames(null, [full, delta], true);
  expect(restart).toBe(false);
  expect(view!.snapshot.quotes.map(item => item.quote?.last)).toEqual(["3500", "3511"]);
  expect(view!.snapshot.catalog?.contracts).toHaveLength(1);
  expect(view!.ticks.filter(tick => tick.symbol === "rb2611").map(tick => tick.price)).toEqual([
    "3510",
    "3511",
  ]);
  const cleared = frame({
    case: "delta",
    value: {
      instanceId: "market",
      sequence: 3n,
      baseSequence: 2n,
      quotes: [{ instrument: { venue: "SHFE", symbol: "rb2611" }, state: "pending" }],
      catalog: { phase: "loading" },
    },
  });
  const after = applyFrames(view, [cleared], true);
  expect(after.view!.snapshot.quotes[1]!.quote).toBeUndefined();
  expect(after.view!.snapshot.catalog?.contracts).toHaveLength(0);
});

test("missing or foreign baselines reopen the stream without applying partial data", () => {
  const { view } = applyFrames(null, [full], true);
  for (const value of [
    { instanceId: "market", sequence: 4n, baseSequence: 3n },
    { instanceId: "restarted", sequence: 2n, baseSequence: 1n },
  ]) {
    const delta = frame({ case: "delta", value });
    expect(applyFrames(view, [delta], true)).toEqual({ view, restart: true });
    expect(applyFrames(null, [delta], true)).toEqual({ view: null, restart: true });
  }
});

test("a new snapshot replaces membership and resets the tick curve", () => {
  const { view } = applyFrames(null, [full], true);
  const reset = frame({
    case: "snapshot",
    value: {
      instanceId: "new",
      sequence: 1n,
      quotes: [row("au2612", "900", 2000n)],
      catalog: { phase: "ready" },
    },
  });
  const after = applyFrames(view, [reset], true);
  expect(after.restart).toBe(false);
  expect(after.view!.snapshot.quotes.map(item => item.instrument!.symbol)).toEqual(["au2612"]);
  expect(after.view!.ticks).toHaveLength(1);
  expect(after.view!.interrupted).toBe(true);
});
