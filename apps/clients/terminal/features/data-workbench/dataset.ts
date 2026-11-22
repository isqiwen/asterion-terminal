import { warehouseData } from "../../src/bridge/warehouse-data";
import { Table, type TableSummary } from "../../src/generated/asterion/warehouse_pb";
import { type MessageValues, translate } from "../../src/i18n";
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
  [Table.WAREHOUSE_RECEIPTS]: "仓单",
  [Table.MEMBER_POSITIONS]: "会员持仓",
  [Table.INVENTORY]: "库存周报",
};
export const tableName = (table: Table) => (names[table] ? t(names[table]!) : t("未知"));
export const exchanges = ["SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"];

// Calendar date in Beijing time (UTC+8), matching Chinese futures trading days.
export const beijingDay = (offset = 0) =>
  new Date(Date.now() + 8 * 3600_000 + offset * 86_400_000).toISOString().slice(0, 10);

// A table's state against its calendar, as the overview shows it.
export function freshness(item: TableSummary | undefined): { tone: Tone; text: string } | null {
  if (!item?.rows) return { tone: "neutral", text: t("未采集") };
  if (!item.expectedLastDay) return null;
  if (!item.missing) return { tone: "ok", text: t("最新") };
  return { tone: "attention", text: t("缺 {count} 个交易日", { count: Number(item.missing) }) };
}

export const startAcquisition = (request: {
  table: Table;
  refetch: boolean;
  exchanges: string[];
  instruments: string[];
  firstDay: string;
  lastDay: string;
}) =>
  warehouseData({
    case: "startAcquisition",
    value: { id: `acq-${crypto.randomUUID().replace(/-/g, "").slice(0, 12)}`, ...request },
  });
