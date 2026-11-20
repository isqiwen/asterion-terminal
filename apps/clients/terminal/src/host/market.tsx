import { fromJson, type JsonValue } from "@bufbuild/protobuf";
import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import { call } from "../bridge/client";
import { marketData } from "../bridge/market-data";
import {
  QuoteFrameSchema,
  CatalogState,
  FeedState,
  Subscription,
  type Board,
  type Minutes,
  type QuoteFrame,
  type QuoteRow,
  type Status,
} from "../generated/asterion/market_pb";
import {
  BackendError,
  asDisplayError,
  isDeselectedService,
  type DisplayError,
} from "../i18n/errors";
import { useErrorNotification } from "./notifications";
import { useIntervalPoll } from "./polling";

// One recent quote for the Tick chart. volume is the contracts traded since
// the contract's previous point of the same trading day.
export type TickPoint = {
  timestamp_ns: string;
  price: string;
  volume?: string;
  day: string;
  total: bigint;
};
// The open Market stream as the page keeps it: every row and the catalog as
// the latest frame left them, and each contract's recent quotes. null when none is open.
export type MarketView = {
  board: Board;
  online: boolean;
  // Why the stream last failed, until a frame arrives.
  error?: string;
  // By rowKey.
  ticks: Map<string, TickPoint[]>;
  // Changes when the quote stream starts afresh.
  generation: number;
  // Per-contract curve resets when trading day or cumulative volume restarts.
  tickGenerations?: Map<string, number>;
  // A new stream started after an earlier one, and its ticks were dropped.
  interrupted: boolean;
  // When Market received the newest quote of any contract.
  lastReceivedMs: number;
};

const Market = createContext<MarketView | null>(null);
export const useMarket = () => useContext(Market);
// Why the saved Market could not be opened, while it still cannot.
const OpenError = createContext<DisplayError>("");
export const useMarketOpenError = () => useContext(OpenError);
export const rowKey = (row: QuoteRow) => `${row.instrument?.exchange}.${row.instrument?.symbol}`;
export const contractGeneration = (market: MarketView, key: string) =>
  `${market.generation}:${market.tickGenerations?.get(key) ?? 0}`;

function tick(view: MarketView, row: QuoteRow) {
  const quote = row.quote;
  if (!quote?.last || quote.exchangeMs === undefined) return;
  const key = rowKey(row);
  let points = view.ticks.get(key) ?? [];
  const previous = points.at(-1);
  const timestamp_ns = String(quote.exchangeMs * 1_000_000n);
  if (previous?.timestamp_ns === timestamp_ns) return;
  let volume: string | undefined;
  if (previous) {
    // A new session or counter never joins the old price curve.
    if (previous.day !== quote.tradingDay || quote.volume < previous.total) {
      points = [];
      const next = (view.tickGenerations?.get(key) ?? 0) + 1;
      (view.tickGenerations ??= new Map()).set(key, next);
    } else volume = String(quote.volume - previous.total);
  }
  view.ticks.set(
    key,
    [
      ...points,
      { timestamp_ns, price: quote.last.text, volume, day: quote.tradingDay, total: quote.volume },
    ].slice(-512),
  );
}

// A full frame replaces the view; a changed frame replaces the rows it holds.
export function applyFrames(previous: MarketView | null, frames: QuoteFrame[], online: boolean) {
  if (frames.length === 0) {
    if (!previous || previous.online === online) return { view: previous, restart: false };
    return { view: { ...previous, online }, restart: false };
  }
  let view: MarketView | null = previous && {
    ...previous,
    ticks: new Map(previous.ticks),
    tickGenerations: new Map(previous.tickGenerations),
    online,
  };
  for (const { frame } of frames) {
    if (frame.case === "full") {
      view = {
        board: frame.value,
        online,
        ticks: new Map(),
        generation: (view?.generation ?? 0) + 1,
        tickGenerations: new Map(),
        interrupted: !!view,
        lastReceivedMs: 0,
      };
    } else if (frame.case === "changed" && view) {
      const board = frame.value;
      const changed = new Map(board.rows.map(row => [rowKey(row), row]));
      view.board = {
        ...view.board,
        feed: board.feed,
        rows: view.board.rows.map(row => changed.get(rowKey(row)) ?? row),
        catalog: board.catalog ?? view.board.catalog,
      };
    } else return { view: previous, restart: true };
    for (const row of frame.value.rows) {
      tick(view, row);
      view.lastReceivedMs = Math.max(view.lastReceivedMs, Number(row.quote?.receivedMs ?? 0));
    }
  }
  return { view, restart: false };
}

// The open watch's frames are read twice a second, hidden window or not.
export function MarketProvider({ endpoint, children }: { endpoint: string; children: ReactNode }) {
  const [view, setView] = useState<MarketView | null>(null);
  const [openError, setOpenError] = useState<DisplayError>("");
  const namespace = "asterion.terminal.futures-market";
  useErrorNotification(openError, namespace);
  const feed = view?.board.feed;
  useErrorNotification(
    feed?.error || feed?.ctpError
      ? new BackendError("unavailable", feed.error || `CTP market rejected: ${feed.ctpError}`)
      : feed?.state === FeedState.CONNECTED
        ? ""
        : undefined,
    namespace,
  );
  const refused = view?.board.rows.filter(row => row.subscription === Subscription.FAILED).length;
  useErrorNotification(
    refused
      ? new BackendError("operation_failed", `CTP subscriptions failed: ${refused}`)
      : view?.online
        ? ""
        : undefined,
    namespace,
  );
  const catalog = view?.board.catalog?.status;
  useErrorNotification(
    catalog?.state === CatalogState.FAILED
      ? new BackendError("operation_failed", catalog.error)
      : catalog?.state === CatalogState.READY
        ? ""
        : undefined,
    namespace,
  );
  useEffect(() => {
    setView(null);
    setOpenError("");
    if (!endpoint) {
      return;
    }
    // Retry absent streams: the host or network may become ready after Terminal.
    let opening = false;
    let tried = Date.now() - 10000;
    const open = async () => {
      opening = true;
      try {
        await call("market.open", {});
      } catch (reason) {
        if (!stopped && !isDeselectedService(reason)) setOpenError(asDisplayError(reason));
      } finally {
        opening = false;
        tried = Date.now();
      }
    };
    let reading = false;
    let stopped = false;
    // This page has no complete frame yet.
    let restart = true;
    let current: MarketView | null = null;
    const read = async () => {
      if (reading) return;
      reading = true;
      try {
        const { market_watch: watch } = (await call("market.watch", { restart })) as {
          market_watch: null | { frames: JsonValue[]; online: boolean; error?: string };
        };
        if (stopped) return;
        restart = false;
        if (!watch) {
          current = null;
          if (!opening && Date.now() - tried >= 10000) void open();
        } else {
          if (watch.error) setOpenError(new BackendError("unavailable", watch.error));
          else if (watch.online) setOpenError("");
          else if (current?.online) setOpenError("行情连接中断，正在重连。");
          const frames = watch.frames.map(frame => fromJson(QuoteFrameSchema, frame));
          ({ view: current, restart } = applyFrames(current, frames, watch.online));
          if (current && current.error !== watch.error)
            current = { ...current, error: watch.error };
        }
        setView(current);
      } catch (reason) {
        if (stopped || isDeselectedService(reason)) return;
        if (current && current.online) current = { ...current, online: false };
        setOpenError(asDisplayError(reason));
        setView(current);
      } finally {
        reading = false;
      }
    };
    void read();
    const timer = window.setInterval(read, 500);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [endpoint]);
  return (
    <Market.Provider value={view}>
      <OpenError.Provider value={openError}>{children}</OpenError.Provider>
    </Market.Provider>
  );
}

// Polls Market service status while online; reconnecting or closing discards pending replies.
export function useMarketStatus(online: boolean): Status | null {
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState<DisplayError>();
  const pending = useRef(false);
  const epoch = useRef(0);
  useErrorNotification(error, "asterion.terminal.futures-market");
  const read = () => {
    if (!online || pending.current) return;
    pending.current = true;
    const current = epoch.current;
    void marketData({ case: "getStatus", value: {} })
      .then(next => {
        if (epoch.current !== current) return;
        setStatus(next);
        setError("");
      })
      .catch(reason => {
        if (epoch.current !== current) return;
        setStatus(null);
        setError(asDisplayError(reason));
      })
      .finally(() => {
        if (epoch.current === current) pending.current = false;
      });
  };
  useEffect(() => {
    epoch.current += 1;
    pending.current = false;
    setStatus(null);
    setError(undefined);
    if (online) read();
    return () => {
      epoch.current += 1;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [online]);
  useIntervalPoll(read, online ? 3000 : null, [online]);
  return status;
}

// Polls one contract's intraday minute bars while mounted; stream restarts reset the series.
export function useMarketMinutes(market: MarketView, row: QuoteRow) {
  const [series, setSeries] = useState<Minutes | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const { exchange, symbol } = row.instrument!;
  const generation = contractGeneration(market, rowKey(row));
  const online = market.online;
  const pending = useRef(false);
  const epoch = useRef(0);
  const load = () => {
    if (!online || pending.current) return;
    pending.current = true;
    const current = epoch.current;
    void marketData({ case: "getMinutes", value: { exchange, symbol } })
      .then(next => {
        if (epoch.current !== current) return;
        setSeries(next);
        setError("");
      })
      .catch(reason => {
        if (epoch.current === current) setError(asDisplayError(reason));
      })
      .finally(() => {
        if (epoch.current === current) pending.current = false;
      });
  };
  useEffect(() => {
    setSeries(null);
    setError("");
  }, [exchange, symbol, generation]);
  useEffect(() => {
    epoch.current += 1;
    pending.current = false;
    if (online) load();
    return () => {
      epoch.current += 1;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [exchange, symbol, generation, online]);
  useIntervalPoll(load, online ? 3000 : null, [exchange, symbol, generation, online]);
  useErrorNotification(error, "asterion.terminal.futures-market");
  return { series, error };
}
