import { fromJson, type JsonValue } from "@bufbuild/protobuf";
import { createContext, useContext, useEffect, useState, type ReactNode } from "react";
import { call } from "../bridge/client";
import {
  QuoteFrameSchema,
  type Board,
  type QuoteFrame,
  type QuoteRow,
} from "../generated/asterion/market_pb";
import { asDisplayError, type DisplayError } from "../i18n/errors";
import { notify } from "./notifications";

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
  // Changes whenever ticks stop continuing the previous curve.
  generation: number;
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
      view.generation++;
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
  let view: MarketView | null = previous && { ...previous, ticks: new Map(previous.ticks), online };
  for (const { frame } of frames) {
    if (frame.case === "full") {
      view = {
        board: frame.value,
        online,
        ticks: new Map(),
        generation: (view?.generation ?? 0) + 1,
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
export function MarketProvider({
  configured,
  children,
}: {
  configured: boolean;
  children: ReactNode;
}) {
  const [view, setView] = useState<MarketView | null>(null);
  const [openError, setOpenError] = useState<DisplayError>("");
  useEffect(() => {
    // Until the saved Market has been reached once, opening it is tried again
    // every ten seconds: its host may start, or the network come up, after
    // Terminal. Once reached, the watch reconnects itself and a disconnect is the user's.
    let reached = false;
    let opening = false;
    let tried = Date.now();
    const open = async () => {
      opening = true;
      try {
        await call("market.open", {});
        if (!stopped) setOpenError("");
      } catch (reason) {
        if (!stopped) setOpenError(asDisplayError(reason));
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
    // A connection that drops is reported once, when it drops.
    let online = false;
    const report = (view: MarketView | null) => {
      if (online && !view?.online)
        notify("行情连接中断，正在重连。", {
          namespace: "asterion.terminal.futures-market",
          severity: "warning",
        });
      online = !!view?.online;
    };
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
          if (configured && !reached && !opening && Date.now() - tried >= 10000) void open();
        } else {
          if (!reached) setOpenError("");
          reached = true;
          const frames = watch.frames.map(frame => fromJson(QuoteFrameSchema, frame));
          ({ view: current, restart } = applyFrames(current, frames, watch.online));
          if (current && current.error !== watch.error)
            current = { ...current, error: watch.error };
        }
        report(current);
        setView(current);
      } catch {
        if (current && current.online) current = { ...current, online: false };
        report(current);
        if (!stopped) setView(current);
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
  }, [configured]);
  return (
    <Market.Provider value={view}>
      <OpenError.Provider value={openError}>{children}</OpenError.Provider>
    </Market.Provider>
  );
}
