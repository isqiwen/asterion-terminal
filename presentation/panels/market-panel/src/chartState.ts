export type ChartViewport = {
  snapshot: string;
  contract: string;
  from: number;
  to: number;
};
export function parseViewport(value: unknown): ChartViewport | null {
  if (!value || typeof value !== "object") return null;
  const v = value as ChartViewport;
  return typeof v.snapshot === "string" &&
    typeof v.contract === "string" &&
    Number.isFinite(v.from) &&
    Number.isFinite(v.to) &&
    v.from < v.to &&
    Math.abs(v.from) < 1e9 &&
    Math.abs(v.to) < 1e9
    ? { snapshot: v.snapshot, contract: v.contract, from: v.from, to: v.to }
    : null;
}
