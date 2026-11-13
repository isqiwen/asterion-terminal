import { useCallback, useEffect, useRef, useState } from "react";
import { Table, type TableSummary } from "../../src/generated/asterion/warehouse_pb";
import { type MessageValues, translate } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import type { Tone } from "../../src/ui/StatusBadge";

export const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);

const names: Partial<Record<Table, string>> = {
  [Table.DAILY]: "日线",
  [Table.MINUTES]: "分钟线",
  [Table.SETTLEMENT]: "结算参数",
  [Table.PRICE_TICKS]: "历史报价单位",
  [Table.CONTRACTS]: "合约表",
  [Table.CALENDAR]: "交易日历",
};
// Market data first, then the parameters and reference tables behind it.
export const tableOrder = [
  Table.DAILY,
  Table.MINUTES,
  Table.SETTLEMENT,
  Table.PRICE_TICKS,
  Table.CONTRACTS,
  Table.CALENDAR,
];
export const tableName = (table: Table) => t(names[table] ?? Table[table]);
export const exchanges = ["SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"];

// A table's state against its calendar, as the overview shows it.
export function freshness(item: TableSummary | undefined): { tone: Tone; text: string } | null {
  if (!item?.rows) return { tone: "neutral", text: t("未采集") };
  if (!item.expectedLastDay) return null;
  if (!item.missing) return { tone: "ok", text: t("最新") };
  return { tone: "attention", text: t("缺 {count} 个交易日", { count: Number(item.missing) }) };
}

// Reads `load` now and, unless `once`, every few seconds: other clients and the
// daily update change the dataset at any time. A slow read is never overlapped.
export function usePolled<T>(load: () => Promise<T>, deps: unknown[], once = false) {
  const [value, setValue] = useState<T>();
  const [error, setError] = useState<DisplayError>("");
  const pending = useRef(false);
  const mounted = useRef(true);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const read = useCallback(load, deps);
  const refresh = useCallback(async () => {
    if (pending.current) return;
    pending.current = true;
    try {
      const next = await read();
      if (mounted.current) {
        setValue(next);
        setError("");
      }
    } catch (failure) {
      if (mounted.current) setError(asDisplayError(failure));
    } finally {
      pending.current = false;
    }
  }, [read]);
  useEffect(() => {
    mounted.current = true;
    void refresh();
    const timer = once ? undefined : setInterval(() => void refresh(), 3000);
    return () => {
      mounted.current = false;
      clearInterval(timer);
    };
  }, [refresh, once]);
  return { value, error, refresh };
}
