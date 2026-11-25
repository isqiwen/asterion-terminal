import { openPopover } from "../testing/popover";
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

test("an initial protocol mismatch shows a status card with settings entry and notifies once across retries", async () => {
  vi.useFakeTimers();
  vi.mocked(call).mockImplementation(async method => {
    if (method === "market.open")
      throw new BackendError("conflict", "market protocol differs from this Terminal");
    return { market_watch: null };
  });
  const openSettings = vi.fn();
  const opens = () => vi.mocked(call).mock.calls.filter(([method]) => method === "market.open");
  render(
    <MarketProvider endpoint="market:7440/fingerprint">
      <main>
        <MarketUnavailable
          context={{
            snapshot: readySnapshot(),
            busy: false,
            error: "",
            navigate: vi.fn(),
            openSettings,
            trade: vi.fn(),
          }}
        />
      </main>
      <NotificationCenter />
    </MarketProvider>,
  );
  await act(async () => {});
  expect(opens()).toHaveLength(1);
  const alert = within(screen.getByRole("main")).getByRole("alert");
  expect(within(alert).getByText("行情服务")).toBeTruthy();
  expect(within(alert).getByText("服务未连接")).toBeTruthy();
  expect(within(alert).getByText("research:7451")).toBeTruthy();
  expect(alert.textContent).toContain("行情服务与 Terminal 的通信协议不一致");
  fireEvent.click(within(alert).getByRole("button", { name: "行情服务设置" }));
  expect(openSettings).toHaveBeenCalledWith("market");
  openPopover(screen.getByRole("button", { expanded: false }));
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
  vi.setSystemTime(new Date("2026-10-13T09:30:00+08:00"));
  let feed = FeedState.RECONNECTING;
  let catalog = CatalogState.FAILED;
  let refused = true;
  const frame = () =>
    toJson(
      QuoteFrameSchema,
      create(QuoteFrameSchema, {
        frame: {
          case: "full",
          value: {
            feed: { state: feed, ctpError: feed === FeedState.RECONNECTING ? 8 : 0 },
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
    <MarketProvider endpoint="market:7440/fingerprint">
      <NotificationCenter />
    </MarketProvider>,
  );
  await act(async () => {});
  openPopover(screen.getByRole("button", { expanded: false }));
  const panel = screen.getByRole("region", { name: "通知" });
  expect(panel.querySelectorAll("li")).toHaveLength(3);
  expect(panel.querySelectorAll("li.warning")).toHaveLength(2);
  expect(panel.querySelectorAll("li.error")).toHaveLength(1);
  expect(panel.textContent).toContain("CTP 8");
  expect(panel.textContent).toContain("1 个合约订阅失败");
  await act(() => vi.advanceTimersByTimeAsync(1500));
  feed = FeedState.CONNECTING;
  catalog = CatalogState.LOADING;
  await act(() => vi.advanceTimersByTimeAsync(500));
  feed = FeedState.RECONNECTING;
  catalog = CatalogState.FAILED;
  await act(() => vi.advanceTimersByTimeAsync(500));
  expect(panel.querySelectorAll("li")).toHaveLength(3);
  expect(panel.querySelector("ol")!.textContent).not.toContain("×");
  feed = FeedState.CONNECTED;
  catalog = CatalogState.READY;
  refused = false;
  await act(() => vi.advanceTimersByTimeAsync(500));
  fireEvent.click(screen.getByRole("button", { name: "清空" }));
  feed = FeedState.RECONNECTING;
  await act(() => vi.advanceTimersByTimeAsync(500));
  expect(panel.querySelectorAll("li")).toHaveLength(1);
  expect(panel.querySelectorAll("li.warning")).toHaveLength(1);
  expect(panel.textContent).toContain("CTP 8");
  fireEvent.click(screen.getByRole("button", { name: "清空" }));
});

test("off-hours recoverable broker timeouts stay quiet while fatal rejections still report errors", async () => {
  vi.useFakeTimers();
  vi.setSystemTime(new Date("2026-10-13T17:00:00+08:00"));
  let feedState = FeedState.RECONNECTING;
  let feedError = "CTP market front connection timed out";
  let feedCtp = 0;
  const frame = () =>
    toJson(
      QuoteFrameSchema,
      create(QuoteFrameSchema, {
        frame: {
          case: "full",
          value: {
            feed: { state: feedState, error: feedError, ctpError: feedCtp },
            catalog: {
              status: {
                state: CatalogState.FAILED,
                error: "CTP catalog front connection timed out",
              },
            },
            rows: [],
          },
        },
      }),
    );
  vi.mocked(call).mockImplementation(async () => ({
    market_watch: { frames: [frame()], online: true },
  }));
  render(
    <MarketProvider endpoint="market:7440/fingerprint">
      <NotificationCenter />
    </MarketProvider>,
  );
  await act(async () => {});
  openPopover(screen.getByRole("button", { expanded: false }));
  const panel = screen.getByRole("region", { name: "通知" });
  expect(panel.textContent).toContain("没有消息");

  feedState = FeedState.FAILED;
  feedError = "";
  feedCtp = 3;
  await act(() => vi.advanceTimersByTimeAsync(500));
  expect(panel.querySelectorAll("li.error")).toHaveLength(1);
  expect(panel.textContent).toContain("CTP 3");
  fireEvent.click(screen.getByRole("button", { name: "清空" }));
});

test("a newly saved offline Market endpoint resumes opening after its failed first attempt", async () => {
  vi.useFakeTimers();
  let endpoint = "first";
  vi.mocked(call).mockImplementation(async method => {
    if (method === "market.open") throw new Error("new Market offline");
    return { market_watch: endpoint === "first" ? { frames: [], online: true } : null };
  });
  const main = render(
    <MarketProvider endpoint={endpoint}>
      <span>first endpoint</span>
    </MarketProvider>,
  );
  await act(() => vi.advanceTimersByTimeAsync(0));
  // Saving a different endpoint clears the native watch.
  endpoint = "second";
  main.rerender(
    <MarketProvider endpoint={endpoint}>
      <span>second endpoint</span>
    </MarketProvider>,
  );
  await act(() => vi.advanceTimersByTimeAsync(21000));
  expect(vi.mocked(call).mock.calls.filter(([method]) => method === "market.open").length).toBe(3);
});

test("a missing quote stream reconnects automatically to the saved endpoint", async () => {
  vi.useFakeTimers();
  let connected = true;
  vi.mocked(call).mockImplementation(async method => {
    if (method === "market.open") connected = true;
    return { market_watch: connected ? { frames: [], online: true } : null };
  });
  render(
    <MarketProvider endpoint="market:7440/fingerprint">
      <span />
    </MarketProvider>,
  );
  await act(async () => {});
  connected = false;
  await act(() => vi.advanceTimersByTimeAsync(21000));
  expect(vi.mocked(call).mock.calls.filter(([method]) => method === "market.open")).toHaveLength(1);
});
