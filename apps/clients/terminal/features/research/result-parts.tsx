import { useRef, type ReactNode } from "react";
import { Period } from "../../src/generated/asterion/common_pb";
import { type MessageValues, translate } from "../../src/i18n";
import type { Tone } from "../../src/ui/StatusBadge";
import { useVirtualRows } from "../../src/ui/virtual-rows";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

export type BenchmarkData = {
  days: string[];
  nav: number[];
  total_return?: number | null;
  max_drawdown?: number | null;
  sharpe?: number | null;
};

// The figures every run has, in the order the result shows them.
export const figures = [
  ["total_return", "总收益", true],
  ["annual_return", "年化收益", true],
  ["sharpe", "夏普比率", false],
  ["max_drawdown", "最大回撤", true],
  ["calmar", "卡玛比率", false],
  ["annual_volatility", "年化波动", true],
  ["winning_days", "上涨日占比", true],
] as const;

const prefixes: Record<string, string> = {
  development_: "留出前",
  holdout_: "留出段",
  out_of_sample_: "样本外",
};

// A metric's name as the page shows it: 夏普比率, 留出前 夏普比率, …
export function metricLabel(name: string) {
  const prefix = Object.keys(prefixes).find(item => name.startsWith(item)) ?? "";
  const base = figures.find(([key]) => key === name.slice(prefix.length));
  if (!base) return name;
  return prefix ? `${t(prefixes[prefix])} ${t(base[1])}` : t(base[1]);
}

export const percentMetric = (name: string) => /return|drawdown|volatility|winning/.test(name);

// Gains and losses take the quote colours.
export function signed(value: number | undefined | null) {
  if (value === undefined || value === null || value === 0) return "";
  return value > 0 ? "lab-up" : "lab-down";
}

export function metric(value: number | undefined, percent: boolean) {
  if (value === undefined) return "—";
  return percent ? `${(value * 100).toFixed(2)}%` : value.toFixed(3);
}

export const periodText = (period: Period) =>
  period === Period.DAY
    ? t("日线")
    : t("{count} 分钟", { count: Period[period]?.replace("MINUTES_", "") ?? "?" });

// A moment as month-day and time in Beijing time.
export function shortTime(ms: bigint) {
  return new Date(Number(ms))
    .toLocaleString("sv-SE", { timeZone: "Asia/Shanghai", hour12: false })
    .slice(5, 16);
}

// When a minute bar ended, Beijing time.
export function minute(timeNs: bigint) {
  return new Date(Number(timeNs / 1_000_000n) + 8 * 3600_000)
    .toISOString()
    .slice(5, 16)
    .replace("T", " ");
}

export function day(timeNs: bigint) {
  // Settlements and bars are labelled at 15:00 Beijing time.
  return new Date(Number(timeNs / 1_000_000n) + 8 * 3600_000).toISOString().slice(0, 10);
}

export const states: Record<string, string> = {
  running: "运行中",
  succeeded: "已完成",
  failed: "失败",
  stopped: "已停止",
  interrupted: "已中断",
};

export const runTones: Record<string, Tone> = {
  succeeded: "ok",
  failed: "danger",
  interrupted: "attention",
};

export function VirtualScrollTable<T>({
  label,
  columns,
  headers,
  rows,
  renderRow,
}: {
  label: string;
  columns: number;
  headers: ReactNode;
  rows: readonly T[];
  renderRow: (row: T, index: number) => ReactNode;
}) {
  const parentRef = useRef<HTMLDivElement>(null);
  const { start, end, topPad, bottomPad } = useVirtualRows(parentRef, rows.length);
  return (
    <div
      ref={parentRef}
      className="lab-virtual-scroll"
      tabIndex={0}
      role="region"
      aria-label={label}
    >
      <table className="lab-table">
        <thead>{headers}</thead>
        <tbody>
          {topPad > 0 && (
            <tr data-spacer aria-hidden="true">
              <td colSpan={columns} style={{ height: topPad }} />
            </tr>
          )}
          {rows.slice(start, end).map((row, offset) => renderRow(row, start + offset))}
          {bottomPad > 0 && (
            <tr data-spacer aria-hidden="true">
              <td colSpan={columns} style={{ height: bottomPad }} />
            </tr>
          )}
        </tbody>
      </table>
    </div>
  );
}
