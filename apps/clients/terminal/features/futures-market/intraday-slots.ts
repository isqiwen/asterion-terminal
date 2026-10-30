import type { IntradaySlot } from "@asterion/client-ui/IntradayChart";
import type { IntradaySeries } from "../../src/bridge/client";
import sessionFile from "../../../../../config/futures-sessions.json";

// Display layout only: the reviewed session reference places observed minutes
// on the trading-day axis. Unknown products fall back to observed minutes.
type Range = readonly [string, string];
const sessions = sessionFile as unknown as {
  day_sessions: Record<string, [string, string][]>;
  night_sessions: Record<string, [string, string]>;
  products: Record<string, Record<string, [string, string | null]>>;
};
const minuteOf = (text: string) => Number(text.slice(0, 2)) * 60 + Number(text.slice(3, 5));
const label = (minute: number) => {
  const value = ((minute % 1440) + 1440) % 1440;
  return `${String(Math.floor(value / 60)).padStart(2, "0")}:${String(value % 60).padStart(2, "0")}`;
};
// Bars carry UTC epoch starts; exchange sessions are defined in UTC+8.
const shanghaiMinute = (ms: number) => (Math.floor(ms / 60000) + 480) % 1440;

export function sessionRanges(venue: string, product: string): Range[] | null {
  const entry = sessions.products[venue]?.[product];
  if (!entry) return null;
  const [day, night] = entry;
  const days = sessions.day_sessions[day];
  if (!days) return null;
  const nights = night ? sessions.night_sessions[night] : undefined;
  return [...(nights ? [nights] : []), ...days];
}

type Merged = { price: string; average: string | null; volume: number };
function merge(target: Map<number, Merged>, index: number, bar: IntradaySeries["bars"][number]) {
  const previous = target.get(index);
  target.set(index, {
    price: bar.close,
    average: bar.average_price ?? previous?.average ?? null,
    volume: (previous?.volume ?? 0) + bar.volume,
  });
}
function fill(count: number, values: Map<number, Merged>, labels: (index: number) => string) {
  const observed = [...values.keys()];
  const first = Math.min(...observed),
    last = Math.max(...observed);
  const slots: IntradaySlot[] = [];
  let carried: Merged | null = null;
  for (let index = 0; index < count; ++index) {
    const value = values.get(index);
    // Minutes without observations inside the observed range repeat the last
    // price with no volume; nothing is drawn before or after observation.
    if (value) carried = value;
    const inside = observed.length > 0 && index >= first && index <= last;
    slots.push({
      label: labels(index),
      price: inside && carried ? carried.price : null,
      average: inside && carried ? carried.average : null,
      volume: value ? value.volume : null,
    });
  }
  return slots;
}

export function intradaySlots(series: IntradaySeries, ranges: Range[] | null) {
  if (ranges) {
    const minutes: number[] = [];
    const ticks: { index: number; label: string }[] = [];
    const starts: number[] = [];
    const ends: number[] = [];
    for (const [start, end] of ranges) {
      const from = minuteOf(start);
      let to = minuteOf(end);
      if (to <= from) to += 1440;
      starts.push(minutes.length);
      ticks.push({ index: minutes.length, label: start });
      for (let minute = from; minute < to; ++minute) minutes.push(minute % 1440);
      ends.push(minutes.length - 1);
    }
    ticks.push({ index: minutes.length - 1, label: ranges[ranges.length - 1][1] });
    const position = new Map(minutes.map((minute, index) => [minute, index]));
    const values = new Map<number, Merged>();
    let placed = true;
    for (const bar of series.bars) {
      const minute = shanghaiMinute(bar.start_ms);
      let index = position.get(minute);
      if (index === undefined)
        // Opening-auction minutes join the first minute; closing ticks the last.
        ranges.forEach(([start, end], session) => {
          const open = minuteOf(start),
            close = minuteOf(end);
          if (minute === close) index = ends[session];
          else if ((open - minute + 1440) % 1440 <= 5) index = starts[session];
        });
      if (index === undefined) {
        placed = false;
        break;
      }
      merge(values, index, bar);
    }
    // Never hide an observation: an unexpected minute uses the observed axis.
    if (placed)
      return { slots: fill(minutes.length, values, index => label(minutes[index])), ticks };
  }
  const values = new Map<number, Merged>();
  series.bars.forEach((bar, index) => merge(values, index, bar));
  const labels = series.bars.map(bar => label(shanghaiMinute(bar.start_ms)));
  const count = series.bars.length;
  const ticks = count
    ? [...new Set([0, Math.floor(count / 3), Math.floor((2 * count) / 3), count - 1])].map(
        index => ({ index, label: labels[index] }),
      )
    : [];
  return { slots: fill(count, values, index => labels[index]), ticks };
}
