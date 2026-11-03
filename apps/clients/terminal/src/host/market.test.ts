import { create, type MessageInitShape } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import { SnapshotSchema } from "../generated/asterion/market_pb";
import { applyFrames } from "./market";

const row = (symbol: string, last: string, sourceMs: bigint) => ({
  instrument: { venue: "SHFE", symbol },
  state: "subscribed",
  quote: { instrument: { venue: "SHFE", symbol }, last, sourceMs, tradingDay: "20261013" },
});
const frame = (init: MessageInitShape<typeof SnapshotSchema>) =>
  create(SnapshotSchema, { instanceId: "market", phase: "connected", ...init });

test("deltas replace their rows in place, keep the catalog, and a gap asks for a new stream", () => {
  const full = frame({
    sequence: 1n,
    subscriptions: [row("rb2610", "3500", 1000n), row("rb2611", "3510", 1000n)],
    catalog: { phase: "ready", contracts: [{ instrument: { venue: "SHFE", symbol: "rb2610" } }] },
  });
  const delta = frame({
    sequence: 2n,
    baseSequence: 1n,
    subscriptionsDelta: true,
    catalogOmitted: true,
    subscriptions: [row("rb2611", "3511", 2000n)],
  });
  const { view, restart } = applyFrames(null, [full, delta], true);
  expect(restart).toBe(false);
  expect(view!.snapshot.subscriptions.map(item => item.quote?.last)).toEqual(["3500", "3511"]);
  expect(view!.snapshot.catalog?.contracts).toHaveLength(1);
  expect(view!.ticks.filter(tick => tick.symbol === "rb2611").map(tick => tick.price)).toEqual([
    "3510",
    "3511",
  ]);
  const skipped = frame({ ...delta, sequence: 4n, baseSequence: 3n });
  const after = applyFrames(view, [skipped], true);
  expect(after.restart).toBe(true);
  expect(after.view).toBe(view);
});
