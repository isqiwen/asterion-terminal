import { create } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import {
  BoardSchema,
  FeedState,
  QuoteRowSchema,
  Subscription,
} from "../../src/generated/asterion/market_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type { MarketView } from "../../src/host/market";
import { quoteStatus } from "./QuoteTable";
import { feature } from "./feature";

registerTerminalFeatures([feature]);

const now = 1_000_000;
// A contract last quoted a minute ago.
const quiet = create(QuoteRowSchema, {
  instrument: { exchange: "SHFE", symbol: "rb2701" },
  subscription: Subscription.ACTIVE,
  quote: { exchangeMs: BigInt(now - 60000), receivedMs: BigInt(now - 60000) },
});
const market = (lastReceivedMs: number): MarketView => ({
  board: create(BoardSchema, { feed: { state: FeedState.CONNECTED }, rows: [quiet] }),
  online: true,
  ticks: new Map(),
  generation: 1,
  interrupted: false,
  lastReceivedMs,
});

test("a quiet contract is not a stale feed; only the whole feed going quiet is", () => {
  expect(quoteStatus(market(now - 1000), quiet, now)).toBe("暂无新报价");
  expect(quoteStatus(market(now - 60000), quiet, now)).toBe("行情未更新");
});
