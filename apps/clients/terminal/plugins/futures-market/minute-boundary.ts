import type { IntradaySeries } from "../../src/bridge/client";
const minute = 60000;
// Received snapshots replace earlier snapshots. Never sort, invent or append
// bars here: the native protocol guarantees unique increasing start labels.
export function observedRuns(series: IntradaySeries | null) {
  if (!series) return null;
  const runs: IntradaySeries["bars"][] = [];
  const gaps: { after_ms: number; before_ms: number; minutes: number }[] = [];
  let previous = 0;
  for (const bar of series.bars) {
    if (
      !Number.isSafeInteger(bar.start_ms) ||
      bar.start_ms <= previous ||
      bar.start_ms % minute !== 0
    )
      return null;
    if (!previous || bar.start_ms !== previous + minute) {
      if (previous)
        gaps.push({
          after_ms: previous + minute,
          before_ms: bar.start_ms,
          minutes: (bar.start_ms - previous - minute) / minute,
        });
      runs.push([]);
    }
    runs[runs.length - 1].push(bar);
    previous = bar.start_ms;
  }
  return { runs, gaps };
}
// Archive APIs expose a version's label, not that version's timestamp
// semantics. Comparing labels is useful, but is NOT interval equivalence.
export function labelBoundary(archiveLast: string | null, series: IntradaySeries | null) {
  const runs = observedRuns(series);
  if (!archiveLast || !runs?.runs.length) return { kind: "unknown" as const, gap_minutes: null };
  const label = BigInt(archiveLast),
    first = BigInt(runs.runs[0][0].start_ms) * 1000000n;
  if (label === first) return { kind: "same_label" as const, gap_minutes: null };
  if (label > first) return { kind: "potential_overlap" as const, gap_minutes: null };
  const distance = first - label,
    step = 60000000000n;
  return {
    kind: "label_gap" as const,
    gap_minutes: distance % step === 0n ? String(distance / step) : null,
  };
}
