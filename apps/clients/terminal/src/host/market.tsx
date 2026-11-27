import { fromBinary, fromJson, type JsonValue } from "@bufbuild/protobuf";
import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import sessionFile from "../../../../../core/config/futures-sessions.json";
import { call } from "../bridge/client";
import { desktop, nativeDesktop } from "../bridge/desktop";
import { marketData } from "../bridge/market-data";
import {
  QuoteFrameSchema,
  CatalogState,
  FeedState,
  Subscription,
  type Board,
  type Catalog,
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

const TICK_CAPACITY = 512;

class TickRing {
  private readonly slots: (TickPoint | undefined)[] = new Array(TICK_CAPACITY);
  private head = 0;
  private count = 0;
  private cached: TickPoint[] | null = null;

  last(): TickPoint | undefined {
    return this.count ? this.slots[(this.head + this.count - 1) & (TICK_CAPACITY - 1)] : undefined;
  }

  clear() {
    this.head = 0;
    this.count = 0;
    this.cached = null;
  }

  push(point: TickPoint) {
    if (this.count < TICK_CAPACITY) {
      this.slots[(this.head + this.count) & (TICK_CAPACITY - 1)] = point;
      this.count += 1;
    } else {
      this.slots[this.head] = point;
      this.head = (this.head + 1) & (TICK_CAPACITY - 1);
    }
    this.cached = null;
  }

  toArray(): TickPoint[] {
    if (this.cached) return this.cached;
    const out = new Array<TickPoint>(this.count);
    for (let i = 0; i < this.count; ++i)
      out[i] = this.slots[(this.head + i) & (TICK_CAPACITY - 1)]!;
    this.cached = out;
    return out;
  }
}

export interface TickLookup {
  readonly size: number;
  get(key: string): TickPoint[] | undefined;
  has(key: string): boolean;
}

class TickStore implements TickLookup {
  private readonly rings = new Map<string, TickRing>();

  get size(): number {
    return this.rings.size;
  }

  has(key: string): boolean {
    return this.rings.has(key);
  }

  get(key: string): TickPoint[] | undefined {
    return this.rings.get(key)?.toArray();
  }

  ring(key: string): TickRing {
    let entry = this.rings.get(key);
    if (!entry) {
      entry = new TickRing();
      this.rings.set(key, entry);
    }
    return entry;
  }
}

// The open Market stream as the page keeps it: every row and the catalog as
// the latest frame left them, and each contract's recent quotes. null when none is open.
export type MarketView = {
  board: Board;
  online: boolean;
  // Why the stream last failed, until a frame arrives.
  error?: string;
  // By rowKey.
  ticks: TickLookup;
  // Changes when the quote stream starts afresh.
  generation: number;
  // Per-contract curve resets when trading day or cumulative volume restarts.
  tickGenerations?: Map<string, number>;
  // A new stream started after an earlier one, and its ticks were dropped.
  interrupted: boolean;
  // When Market received the newest quote of any contract.
  lastReceivedMs: number;
  // Row key -> index in board.rows; rebuilt only on full frames.
  rowIndex?: Map<string, number>;
};

const Market = createContext<MarketView | null>(null);
export const useMarket = () => useContext(Market);
const MarketCatalog = createContext<Catalog | undefined>(undefined);
export const useMarketCatalog = () => useContext(MarketCatalog);
const MarketAvailable = createContext(false);
export const useMarketAvailable = () => useContext(MarketAvailable);
// Why the saved Market could not be opened, while it still cannot.
const OpenError = createContext<DisplayError>("");
export const useMarketOpenError = () => useContext(OpenError);
export const rowKey = (row: QuoteRow) => `${row.instrument?.exchange}.${row.instrument?.symbol}`;
export const contractGeneration = (market: MarketView, key: string) =>
  `${market.generation}:${market.tickGenerations?.get(key) ?? 0}`;

function indexRows(rows: readonly QuoteRow[]) {
  const index = new Map<string, number>();
  for (let i = 0; i < rows.length; ++i) index.set(rowKey(rows[i]!), i);
  return index;
}

function recordTick(
  store: TickStore,
  row: QuoteRow,
  generations: { map?: Map<string, number>; cloned: boolean },
) {
  const quote = row.quote;
  if (!quote?.last || quote.exchangeMs === undefined) return;
  const key = rowKey(row);
  const ring = store.ring(key);
  const previous = ring.last();
  const timestamp_ns = String(quote.exchangeMs * 1_000_000n);
  if (previous?.timestamp_ns === timestamp_ns) return;
  let volume: string | undefined;
  if (previous) {
    // A new session or counter never joins the old price curve.
    if (previous.day !== quote.tradingDay || quote.volume < previous.total) {
      ring.clear();
      if (!generations.cloned) {
        generations.map = new Map(generations.map);
        generations.cloned = true;
      }
      generations.map!.set(key, (generations.map!.get(key) ?? 0) + 1);
    } else volume = String(quote.volume - previous.total);
  }
  ring.push({
    timestamp_ns,
    price: quote.last.text,
    volume,
    day: quote.tradingDay,
    total: quote.volume,
  });
}

// A full frame replaces the view; a changed frame replaces the rows it holds.
export function applyFrames(previous: MarketView | null, frames: QuoteFrame[], online: boolean) {
  if (frames.length === 0) {
    if (!previous || previous.online === online) return { view: previous, restart: false };
    return { view: { ...previous, online }, restart: false };
  }
  let store: TickStore | null =
    previous && previous.ticks instanceof TickStore ? previous.ticks : null;
  let view: MarketView | null = previous && {
    ...previous,
    ticks: store ?? new TickStore(),
    online,
  };
  if (view && !store) store = view.ticks as TickStore;
  const generations = { map: view?.tickGenerations, cloned: false };
  let copiedRows: QuoteRow[] | null = null;
  for (const { frame } of frames) {
    if (frame.case === "full") {
      store = new TickStore();
      generations.map = new Map();
      generations.cloned = true;
      copiedRows = null;
      view = {
        board: frame.value,
        online,
        ticks: store,
        generation: (view?.generation ?? 0) + 1,
        tickGenerations: generations.map,
        interrupted: !!view,
        lastReceivedMs: 0,
        rowIndex: indexRows(frame.value.rows),
      };
    } else if (frame.case === "changed" && view && store) {
      const board = frame.value;
      const index = view.rowIndex ?? indexRows(view.board.rows);
      view.rowIndex = index;
      if (board.rows.length > 0) {
        if (!copiedRows) copiedRows = view.board.rows.slice();
        for (const row of board.rows) {
          const pos = index.get(rowKey(row));
          if (pos !== undefined) copiedRows[pos] = row;
        }
      }
      view.board = {
        ...view.board,
        feed: board.feed,
        rows: copiedRows ?? view.board.rows,
        catalog: board.catalog ?? view.board.catalog,
      };
    } else return { view: previous, restart: true };
    for (const row of frame.value.rows) {
      recordTick(store, row, generations);
      const received = Number(row.quote?.receivedMs ?? 0);
      if (received > view.lastReceivedMs) view.lastReceivedMs = received;
    }
  }
  if (view && generations.cloned) view.tickGenerations = generations.map;
  return { view, restart: false };
}

// Market sessions from core/config/futures-sessions.json are defined in UTC+8.
const MARKET_UTC_OFFSET_MS = 8 * 60 * 60 * 1000;
const SESSION_BUFFER_MINUTES = 15;
const sessions = sessionFile as unknown as {
  day_sessions: Record<string, [string, string][]>;
  night_sessions: Record<string, [string, string]>;
};
const parseMinutes = (hhmm: string) => Number(hhmm.slice(0, 2)) * 60 + Number(hhmm.slice(3, 5));
const dayWindows = Object.values(sessions.day_sessions).flat();
const nightWindows = Object.values(sessions.night_sessions);
const dayOpen =
  Math.min(...dayWindows.map(([start]) => parseMinutes(start))) - SESSION_BUFFER_MINUTES;
const dayClose =
  Math.max(...dayWindows.map(([, end]) => parseMinutes(end))) + SESSION_BUFFER_MINUTES;
const nightOpen =
  Math.min(...nightWindows.map(([start]) => parseMinutes(start))) - SESSION_BUFFER_MINUTES;
const nightClose =
  Math.max(
    ...nightWindows
      .filter(([start, end]) => parseMinutes(end) < parseMinutes(start))
      .map(([, end]) => parseMinutes(end)),
  ) + SESSION_BUFFER_MINUTES;

export function isMarketSessionOpen(now: Date = new Date()): boolean {
  const beijing = new Date(now.getTime() + MARKET_UTC_OFFSET_MS);
  const day = beijing.getUTCDay();
  const minute = beijing.getUTCHours() * 60 + beijing.getUTCMinutes();
  if (day >= 1 && day <= 5 && ((minute >= dayOpen && minute <= dayClose) || minute >= nightOpen))
    return true;
  // Mon-Fri night sessions extend into Tue-Sat early morning.
  return day >= 2 && day <= 6 && minute <= nightClose;
}

const RECOVERABLE_CATALOG_PREFIXES = [
  "CTP catalog front connection timed out",
  "CTP catalog authentication timed out",
  "CTP catalog login timed out",
  "CTP catalog not initialized",
  "CTP catalog front inactive",
  "CTP catalog disconnected",
];

// The open watch's frames are read twice a second, hidden window or not.
export function MarketProvider({ endpoint, children }: { endpoint: string; children: ReactNode }) {
  const [view, setView] = useState<MarketView | null>(null);
  const [openError, setOpenError] = useState<DisplayError>("");
  const namespace = "asterion.terminal.futures-market";
  const inSession = isMarketSessionOpen();
  useErrorNotification(openError, namespace);
  const feed = view?.board.feed;
  const feedDiagnostic =
    feed?.error || (feed?.ctpError ? `CTP market rejected: ${feed.ctpError}` : "");
  const feedRecovering = feed?.state === FeedState.RECONNECTING && Boolean(feedDiagnostic);
  useErrorNotification(
    feed?.state === FeedState.CONNECTED
      ? ""
      : feed?.state === FeedState.FAILED && feedDiagnostic
        ? new BackendError("unavailable", feedDiagnostic)
        : feedRecovering && inSession
          ? new BackendError("unavailable", feedDiagnostic)
          : undefined,
    namespace,
    feedRecovering ? "warning" : "error",
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
  const catalogRecoverable =
    catalog?.state === CatalogState.FAILED &&
    RECOVERABLE_CATALOG_PREFIXES.some(prefix => catalog.error.startsWith(prefix));
  useErrorNotification(
    catalog?.state === CatalogState.READY
      ? ""
      : catalog?.state === CatalogState.FAILED && (!catalogRecoverable || inSession)
        ? new BackendError("operation_failed", catalog.error)
        : undefined,
    namespace,
    catalogRecoverable ? "warning" : "error",
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
    let deferredRender = false;
    // This page has no complete frame yet.
    let restart = true;
    let current: MarketView | null = null;
    let published: MarketView | null = null;
    const statusChanged = (next: MarketView | null, prev: MarketView | null) =>
      Boolean(next) !== Boolean(prev) ||
      next?.online !== prev?.online ||
      next?.error !== prev?.error ||
      next?.board.feed?.state !== prev?.board.feed?.state ||
      next?.board.catalog?.status?.state !== prev?.board.catalog?.status?.state;
    const publish = (next: MarketView | null) => {
      if (document.hidden && !statusChanged(next, published)) {
        deferredRender = true;
        return;
      }
      deferredRender = false;
      published = next;
      setView(next);
    };
    const onVisibility = () => {
      if (!stopped && !document.hidden && deferredRender) {
        deferredRender = false;
        published = current;
        setView(current);
      }
    };
    document.addEventListener("visibilitychange", onVisibility);
    const read = async () => {
      if (reading) return;
      reading = true;
      try {
        const nativeWatch = nativeDesktop ? desktop().marketWatch : undefined;
        let frames: QuoteFrame[] = [];
        let online = false;
        let error: string | undefined;
        let hasWatch = false;
        if (nativeWatch) {
          const batch = await nativeWatch(restart);
          if (stopped) return;
          restart = false;
          if (batch) {
            hasWatch = true;
            online = batch.online;
            error = batch.error || undefined;
            frames = batch.frames.map(bytes => fromBinary(QuoteFrameSchema, bytes));
          }
        } else {
          const { market_watch: watch } = (await call("market.watch", { restart })) as {
            market_watch: null | { frames: JsonValue[]; online: boolean; error?: string };
          };
          if (stopped) return;
          restart = false;
          if (watch) {
            hasWatch = true;
            online = watch.online;
            error = watch.error || undefined;
            frames = watch.frames.map(frame => fromJson(QuoteFrameSchema, frame));
          }
        }
        if (!hasWatch) {
          current = null;
          if (!opening && Date.now() - tried >= 10000) void open();
        } else {
          if (error) setOpenError(new BackendError("unavailable", error));
          else if (online) setOpenError("");
          else if (current?.online) setOpenError("行情连接中断，正在重连。");
          ({ view: current, restart } = applyFrames(current, frames, online));
          if (current && current.error !== error) current = { ...current, error };
        }
        publish(current);
      } catch (reason) {
        if (stopped || isDeselectedService(reason)) return;
        if (current && current.online) current = { ...current, online: false };
        setOpenError(asDisplayError(reason));
        publish(current);
      } finally {
        reading = false;
      }
    };
    void read();
    const timer = window.setInterval(read, 500);
    return () => {
      stopped = true;
      document.removeEventListener("visibilitychange", onVisibility);
      window.clearInterval(timer);
    };
  }, [endpoint]);
  return (
    <MarketAvailable.Provider value={view !== null}>
      <MarketCatalog.Provider value={view?.board.catalog}>
        <Market.Provider value={view}>
          <OpenError.Provider value={openError}>{children}</OpenError.Provider>
        </Market.Provider>
      </MarketCatalog.Provider>
    </MarketAvailable.Provider>
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
