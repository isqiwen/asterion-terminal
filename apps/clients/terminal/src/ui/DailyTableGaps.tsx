import { translate, type MessageValues } from "../i18n";
import type { TableCoverageRow } from "../bridge/client";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// The first and last stored day say nothing of the days between them: what is
// known to be missing there, or that it cannot be told, is said beside them.
export function DailyTableGaps({ stored }: { stored: TableCoverageRow }) {
  if (stored.missing_days === 0) return null;
  return (
    <p role="status">
      {stored.missing_days === null
        ? t(
            "这个范围内的交易日历没有全部入库，中间是否缺交易日无法判断；数据集用到没有入库的交易日时会被拒绝并列出日期。",
          )
        : t("这个范围内有 {count} 个交易日还没有入库；数据集用到这些交易日时会被拒绝并列出日期。", {
            count: stored.missing_days,
          })}
    </p>
  );
}
