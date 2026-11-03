import { fromJson, type JsonValue } from "@bufbuild/protobuf";
import { createContext, useContext, useEffect, useState, type ReactNode } from "react";
import { call } from "../bridge/client";
import { SnapshotSchema, type Snapshot, type Subscription } from "../generated/asterion/market_pb";

// One recent quote for the Tick chart. volume is the contracts traded since
// the contract's previous point of the same trading day.
export type TickPoint = {
  venue: string;
  symbol: string;
  timestamp_ns: string;
  price: string;
  volume?: string;
  day: string;
  total: bigint;
};
// The open Market watch as the page keeps it: the latest frame with every
// row and the catalog filled in, and recent quotes. null when none is open.
export type MarketView = {
  snapshot: Snapshot;
  online: boolean;
  ticks: TickPoint[];
  // Changes whenever ticks stop continuing the previous curve.
  generation: number;
  // A new stream started after an earlier one, and its ticks were dropped.
  interrupted: boolean;
};

const Market = createContext<MarketView | null>(null);
export const useMarket = () => useContext(Market);
export const rowKey = (row: Subscription) => `${row.instrument?.venue}.${row.instrument?.symbol}`;

function tick(view: MarketView, row: Subscription) {
  const quote = row.quote;
  if (!quote?.last || quote.sourceMs <= 0n) return;
  const venue = row.instrument!.venue;
  const symbol = row.instrument!.symbol;
  const same = (point: TickPoint) => point.venue === venue && point.symbol === symbol;
  let previous: TickPoint | undefined;
  for (let i = view.ticks.length - 1; i >= 0 && !previous; --i)
    if (same(view.ticks[i]!)) previous = view.ticks[i];
  if (previous?.timestamp_ns === String(quote.sourceMs * 1_000_000n)) return;
  let volume: string | undefined;
  if (previous) {
    // A new session or counter never joins the old price curve.
    if (previous.day !== quote.tradingDay || quote.volume < previous.total) {
      view.ticks = view.ticks.filter(point => !same(point));
      view.generation++;
    } else volume = String(quote.volume - previous.total);
  }
  view.ticks.push({
    venue,
    symbol,
    timestamp_ns: String(quote.sourceMs * 1_000_000n),
    price: quote.last,
    volume,
    day: quote.tradingDay,
    total: quote.volume,
  });
  if (view.ticks.length > 512) view.ticks.shift();
}

// A complete frame replaces every row; a delta frame replaces the rows it
// carries and follows the previous frame. The catalog is sent again only when
// it changed. A delta without its previous frame needs a new stream (restart).
export function applyFrames(previous: MarketView | null, frames: Snapshot[], online: boolean) {
  let view: MarketView | null = previous && { ...previous, ticks: [...previous.ticks], online };
  for (const frame of frames) {
    if (frame.subscriptionsDelta && view?.snapshot.sequence !== frame.baseSequence)
      return { view: previous, restart: true };
    if (!view || !frame.subscriptionsDelta) {
      // A new stream: quotes between the streams are lost, so the curve starts again.
      view = {
        snapshot: frame,
        online,
        ticks: [],
        generation: (view?.generation ?? 0) + 1,
        interrupted: !!view,
      };
    } else {
      const changed = new Map(frame.subscriptions.map(row => [rowKey(row), row]));
      view.snapshot = {
        ...frame,
        subscriptions: view.snapshot.subscriptions.map(row => changed.get(rowKey(row)) ?? row),
        catalog: frame.catalogOmitted ? view.snapshot.catalog : frame.catalog,
      };
    }
    for (const row of frame.subscriptions) tick(view, row);
  }
  return { view, restart: false };
}

// While the window is visible, the open watch's frames are read twice a second.
export function MarketProvider({ active, children }: { active: boolean; children: ReactNode }) {
  const [view, setView] = useState<MarketView | null>(null);
  useEffect(() => {
    if (!active) return;
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
          market_watch: null | { frames: JsonValue[]; online: boolean };
        };
        if (stopped) return;
        restart = false;
        if (!watch) current = null;
        else {
          const frames = watch.frames.map(frame => fromJson(SnapshotSchema, frame));
          ({ view: current, restart } = applyFrames(current, frames, watch.online));
        }
        setView(current);
      } catch {
        current = current && { ...current, online: false };
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
  }, [active]);
  return <Market.Provider value={view}>{children}</Market.Provider>;
}
