import { create } from "@bufbuild/protobuf";
import { fireEvent, render, screen } from "@testing-library/react";
import type { ReactNode } from "react";
import { expect, test, vi } from "vitest";
import { BoardSchema } from "../../src/generated/asterion/market_pb";
import { registerTerminalFeatures, type TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { MarketBoard } from "./MarketBoard";
import { feature } from "./feature";
import { selectQuote } from "./selected-quote";

vi.mock("./QuotePane", () => ({ QuotePane: ({ actions }: { actions: ReactNode }) => actions }));
registerTerminalFeatures([feature]);

test("ordering from a selected quote passes its exchange and contract to Trading", () => {
  vi.stubGlobal(
    "ResizeObserver",
    class {
      observe() {}
      disconnect() {}
    },
  );
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
