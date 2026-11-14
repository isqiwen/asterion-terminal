import { create, toJson } from "@bufbuild/protobuf";
import { act, fireEvent, render, screen, within } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "./feature-registry";
import { feature } from "../../features/futures-market/feature";
import { MarketUnavailable } from "../../features/futures-market/MarketUnavailable";
import { call } from "../bridge/client";
import {
  CatalogState,
  FeedState,
  QuoteFrameSchema,
  Subscription,
} from "../generated/asterion/market_pb";
import { BackendError } from "../i18n/errors";
import { setLocale } from "../i18n";
import { readySnapshot } from "../testing/core";
import { MarketProvider } from "./market";
import { NotificationCenter } from "./notifications";

vi.mock("../bridge/client", () => ({ call: vi.fn() }));
registerTerminalFeatures([feature]);
beforeEach(() => vi.mocked(call).mockReset());

test("an initial protocol mismatch is notified without occupying the workspace or repeating on retries", async () => {
  vi.useFakeTimers();
  vi.mocked(call).mockImplementation(async method => {
    if (method === "market.open")
      throw new BackendError("conflict", "market protocol differs from this Terminal");
    return { market_watch: null };
  });
  const opens = () => vi.mocked(call).mock.calls.filter(([method]) => method === "market.open");
  render(
    <MarketProvider configured>
      <main>
        <MarketUnavailable
          context={{
            snapshot: readySnapshot(),
            busy: false,
            error: "",
            navigate: vi.fn(),
            openSettings: vi.fn(),
            trade: vi.fn(),
          }}
        />
      </main>
      <NotificationCenter />
    </MarketProvider>,
  );
  await act(async () => {});
  expect(opens()).toHaveLength(1);
  expect(screen.getByRole("main").textContent).toBe("");
  fireEvent.click(screen.getByRole("button", { expanded: false }));
  const panel = screen.getByRole("region", { name: "通知" });
  expect(panel.textContent).toContain("行情服务与 Terminal 的通信协议不一致");
  expect(within(panel).getByTitle(/conflict: market protocol differs/)).toBeTruthy();
  await act(() => vi.advanceTimersByTimeAsync(21000));
  expect(opens()).toHaveLength(3);
  expect(panel.querySelectorAll("li")).toHaveLength(1);
  expect(panel.querySelector("ol")!.textContent).not.toContain("×");
  act(() => setLocale("en-US"));
  expect(panel.textContent).toContain("Update both to the same release");
  fireEvent.click(screen.getByRole("button", { name: "Clear" }));
});

test("broker, catalog and subscription failures are notified off-page, once until recovery", async () => {
  vi.useFakeTimers();
  let feed = FeedState.FAILED;
  let catalog = CatalogState.FAILED;
  let refused = true;
  const frame = () =>
    toJson(
      QuoteFrameSchema,
      create(QuoteFrameSchema, {
        frame: {
          case: "full",
          value: {
            feed: { state: feed, ctpError: feed === FeedState.FAILED ? 8 : 0 },
            catalog: {
              status: {
                state: catalog,
                error:
                  catalog === CatalogState.FAILED
                    ? "CTP catalog front inactive: CTP catalog authentication rejected: 8: CTP:前置不活跃"
                    : "",
              },
            },
            rows: [
              {
                instrument: { exchange: "SHFE", symbol: "rb2701" },
                subscription: refused ? Subscription.FAILED : Subscription.ACTIVE,
              },
            ],
          },
        },
      }),
    );
  vi.mocked(call).mockImplementation(async () => ({
    market_watch: { frames: [frame()], online: true },
  }));
  render(
    <MarketProvider configured>
      <NotificationCenter />
    </MarketProvider>,
  );
  await act(async () => {});
  fireEvent.click(screen.getByRole("button", { expanded: false }));
  const panel = screen.getByRole("region", { name: "通知" });
  expect(panel.querySelectorAll("li")).toHaveLength(3);
  expect(panel.textContent).toContain("CTP 8");
  expect(panel.textContent).toContain("1 个合约订阅失败");
  await act(() => vi.advanceTimersByTimeAsync(1500));
  feed = FeedState.CONNECTING;
  catalog = CatalogState.LOADING;
  await act(() => vi.advanceTimersByTimeAsync(500));
  feed = FeedState.FAILED;
  catalog = CatalogState.FAILED;
  await act(() => vi.advanceTimersByTimeAsync(500));
  expect(panel.querySelectorAll("li")).toHaveLength(3);
  expect(panel.querySelector("ol")!.textContent).not.toContain("×");
  feed = FeedState.CONNECTED;
  catalog = CatalogState.READY;
  refused = false;
  await act(() => vi.advanceTimersByTimeAsync(500));
  fireEvent.click(screen.getByRole("button", { name: "清空" }));
  feed = FeedState.FAILED;
  await act(() => vi.advanceTimersByTimeAsync(500));
  expect(panel.querySelectorAll("li")).toHaveLength(1);
  expect(panel.textContent).toContain("CTP 8");
});
