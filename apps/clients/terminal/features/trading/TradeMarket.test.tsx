import { create } from "@bufbuild/protobuf";
import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { Side } from "../../src/generated/asterion/common_pb";
import { BoardSchema, FeedState, Subscription } from "../../src/generated/asterion/market_pb";
import { SessionSchema } from "../../src/generated/asterion/trading_pb";
import { registerTerminalFeatures, type TerminalContext } from "../../src/host/feature-registry";
import type { MarketView } from "../../src/host/market";
import { feature as market } from "../futures-market/feature";
import { feature } from "./feature";
import { TradeMarket } from "./TradeMarket";

vi.mock("../futures-market/QuotePane", () => ({
  QuotePane: ({ name }: { name: string }) => <p>chart {name}</p>,
}));
registerTerminalFeatures([market, feature]);

const quote = (exchange: string, symbol: string) => ({
  instrument: { exchange, symbol },
  subscription: Subscription.ACTIVE,
  quote: { last: { text: "100" }, exchangeMs: 1n, receivedMs: 1n },
});

test.each(["watchlist", "position"])(
  "missing quotes keep the saved %s visible as unavailable",
  source => {
    const instrument = { exchange: "SHFE", symbol: "rb2610" };
    render(
      <TradeMarket
        context={
          {
            snapshot: {
              market_endpoint: { instruments: source === "watchlist" ? [instrument] : [] },
            },
          } as unknown as TerminalContext
        }
        market={{ board: create(BoardSchema) } as unknown as MarketView}
        session={create(SessionSchema, {
          positions: source === "position" ? [{ instrument }] : [],
        })}
        contract=""
        select={vi.fn()}
      />,
    );
    expect(screen.queryByText("还没有自选合约")).toBeNull();
    expect(screen.getAllByText("无行情")).toHaveLength(2);
  },
);

test("holdings list before the watchlist, and choosing a row moves the contract", () => {
  const view = {
    board: create(BoardSchema, {
      feed: { state: FeedState.CONNECTED },
      rows: [quote("DCE", "i2701"), quote("SHFE", "rb2610"), quote("CZCE", "MA701")],
    }),
    online: true,
    ticks: new Map(),
    generation: 1,
  } as unknown as MarketView;
  const session = create(SessionSchema, {
    positions: [{ instrument: { exchange: "SHFE", symbol: "rb2610" }, side: Side.BUY }],
  });
  const context = {
    snapshot: {
      market_endpoint: {
        instruments: [
          { exchange: "DCE", symbol: "i2701" },
          { exchange: "SHFE", symbol: "rb2610" },
        ],
      },
    },
  } as unknown as TerminalContext;
  const select = vi.fn();
  render(
    <TradeMarket
      context={context}
      market={view}
      session={session}
      contract="SHFE.rb2610"
      select={select}
    />,
  );
  const names = within(screen.getByRole("region"))
    .getAllByRole("button")
    .map(button => button.getAttribute("data-quote-id"));
  expect(names).toEqual(["SHFE.rb2610", "DCE.i2701"]);
  expect(screen.getByText("chart rb2610")).toBeTruthy();
  fireEvent.click(within(screen.getByRole("region")).getAllByRole("button")[1]!);
  expect(select).toHaveBeenCalledWith("DCE.i2701");
});

test("initial selection prefers the first tradable holding or watchlist contract", () => {
  const view = {
    board: create(BoardSchema, {
      feed: { state: FeedState.CONNECTED },
      rows: [quote("SHFE", "au2612"), quote("SHFE", "rb2701")],
    }),
    online: true,
    ticks: new Map(),
    generation: 1,
  } as unknown as MarketView;
  const session = create(SessionSchema, {
    policy: {
      contracts: [
        { instrument: { exchange: "SHFE", symbol: "rb" }, product: "rb", deliveryMonth: "" },
      ],
    },
    positions: [
      { instrument: { exchange: "SHFE", symbol: "au2612" }, side: Side.SELL },
      { instrument: { exchange: "SHFE", symbol: "rb2701" }, side: Side.SELL },
    ],
  });
  const select = vi.fn();
  render(
    <TradeMarket
      context={{ snapshot: null } as unknown as TerminalContext}
      market={view}
      session={session}
      contract=""
      select={select}
    />,
  );
  expect(select).toHaveBeenCalledWith("SHFE.rb2701");
});
