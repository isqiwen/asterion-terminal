import { create } from "@bufbuild/protobuf";
import { fireEvent, render, screen } from "@testing-library/react";
import type { ReactNode } from "react";
import { beforeEach, expect, test, vi } from "vitest";
import { BoardSchema } from "../../src/generated/asterion/market_pb";
import { registerTerminalFeatures, type TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { MarketBoard } from "./MarketBoard";
import { feature } from "./feature";
import { selectQuote } from "./selected-quote";

vi.mock("./QuotePane", () => ({ QuotePane: ({ actions }: { actions: ReactNode }) => actions }));
registerTerminalFeatures([feature]);

beforeEach(() => {
  vi.stubGlobal(
    "ResizeObserver",
    class {
      observe() {}
      disconnect() {}
    },
  );
});

test.each([
  { watched: false, quoted: false, query: "", status: "还没有自选合约" },
  { watched: true, quoted: false, query: "", status: "尚无订阅行情" },
  { watched: true, quoted: true, query: "rb", status: "没有匹配的市场合约" },
])(
  "watchlist empty state follows saved contracts and quotes ($status)",
  ({ watched, quoted, query, status }) => {
    const instrument = { exchange: "CZCE", symbol: "OI701" };
    const drafts = createWorkspaceDrafts();
    drafts.write(JSON.stringify(["market", "market-section"]), "自选");
    drafts.write(JSON.stringify(["market", "quote-search"]), query);
    render(
      <WorkspaceDraftScope store={drafts} namespace="market">
        <MarketBoard
          market={{
            board: create(BoardSchema, { rows: quoted ? [{ instrument }] : [] }),
            online: true,
            ticks: new Map(),
            generation: 0,
            interrupted: true,
            lastReceivedMs: 0,
          }}
          context={
            {
              snapshot: {
                market_endpoint: { host: "market", instruments: watched ? [instrument] : [] },
              },
              busy: false,
            } as unknown as TerminalContext
          }
        />
      </WorkspaceDraftScope>,
    );
    expect(screen.getByRole("status").textContent).toBe(status);
    if (watched) expect(screen.getByRole("button", { name: "移除 OI701" })).toBeTruthy();
  },
);

test("ordering from a selected quote passes its exchange and contract to Trading", () => {
  const navigate = vi.fn();
  selectQuote("DCE.i2701");
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="market">
      <MarketBoard
        market={{
          board: create(BoardSchema, {
            rows: [
              { instrument: { exchange: "SHFE", symbol: "rb2701" } },
              { instrument: { exchange: "DCE", symbol: "i2701" } },
            ],
          }),
          online: true,
          ticks: new Map(),
          generation: 0,
          interrupted: false,
          lastReceivedMs: 0,
        }}
        context={{ snapshot: null, busy: false, navigate } as unknown as TerminalContext}
      />
    </WorkspaceDraftScope>,
  );
  fireEvent.click(screen.getByRole("button", { name: "下单" }));
  expect(navigate).toHaveBeenCalledWith("workspace.trading", { params: { contract: "DCE.i2701" } });
});
