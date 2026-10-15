import type { TablePage } from "../../src/bridge/client";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const reasons: Record<string, string> = {
  raw_missing: "未留存响应，不能反推或补造质量结论。",
  unsupported_scope: "当前仅核对 Tushare SHFE 日线响应与分区。",
  unsupported_response_shape: "响应版本或结构不在本期核对范围内。",
  key_mapping_unverified: "存在无法唯一对应的合约键，相关值保持未知。",
  checked_recorded_partition: "已按已留存响应核对完整记录分区。",
};
const kinds: Record<string, string> = {
  duplicate_response_key: "响应键重复",
  response_key_without_partition: "响应有键，分区无行",
  partition_key_without_response: "分区有键，响应无行",
  not_exactly_representable: "超出目标精度或范围，禁止舍入",
  value_mismatch: "规范化值不同",
};
const fields: [string, string, string][] = [
  ["ts_code", "contract", "从已存完整身份反查来源键；不推断世纪"],
  ["trade_date", "partition.period", "逐行必须等于分区日期"],
  ...["open", "high", "low", "close"].map(
    field => [field, field, "8位小数；零转缺失"] as [string, string, string],
  ),
  ["settle", "settlement", "8位小数"],
  ["pre_close", "previous_close", "8位小数"],
  ["pre_settle", "previous_settlement", "8位小数"],
  ["vol", "volume", "整数手数"],
  ["amount", "amount", "原值乘10000；2位小数"],
  ["oi", "open_interest", "整数手数"],
  ["oi_chg", "open_interest_change", "整数手数"],
  ["delv_settle", "delivery_settlement", "8位小数"],
];
export function ResponseQuality({ review }: { review: TablePage["response_review"] }) {
  const status = review?.status ?? "unknown";
  const shown = review?.differences.slice(0, 100) ?? [];
  const cell = (value: string | null) => (value === null ? t("缺失") : value);
  return (
    <details>
      <summary>{t("响应与分区质量核对")}</summary>
      <section aria-label={t("响应与分区质量核对")}>
        <p role="status">
          {t(
            status === "consistent"
              ? "已核对：记录分区一致"
              : status === "differences"
                ? "发现差异"
                : "证据不足：未知",
          )}
        </p>
        <p>{t(reasons[review?.reason ?? "raw_missing"] ?? "响应版本或结构不在本期核对范围内。")}</p>
        <p className="subtle">
          {t(
            "只读核对完整分区，不限当前分页；精确展开数值，不使用浮点或舍入。不能证明来源目录、历史世纪映射、行情真实性或整日市场完整性。",
          )}
        </p>
        {review && (
          <>
            {review.response_fields > 0 ? (
              <p>
                {t(
                  "源字段 {fields}；响应 {raw} 行；其中4位交割月 {dated} 行；规范化 {stored} 行；对应 {matched} 行；比较 {cells} 个数值单元格。",
                  {
                    fields: review.response_fields,
                    raw: review.response_rows,
                    dated: review.dated_rows,
                    stored: review.normalized_rows,
                    matched: review.matched_rows,
                    cells: review.compared_cells,
                  },
                )}
              </p>
            ) : (
              <p>
                {t("已存分区 {rows} 行；响应字段、行数与对应关系未知。", {
                  rows: review.normalized_rows,
                })}
              </p>
            )}
            <p>
              {t("按现有规则排除的非日期合约键")} ({review.skipped_keys.length}):{" "}
              {review.skipped_keys.join(", ") || "—"}
            </p>
            <p>
              {t("对应关系未知的键")} ({review.unknown_keys.length}):{" "}
              {review.unknown_keys.join(", ") || "—"}
            </p>
            {review.differences.length > 0 && (
              <>
                <p>
                  {t("差异共 {total} 项；显示前 {shown} 项。", {
                    total: review.differences.length,
                    shown: shown.length,
                  })}
                </p>
                <div className="history-usage-table">
                  <table aria-label={t("响应分区差异")}>
                    <thead>
                      <tr>
                        {[
                          "差异类型",
                          "来源键",
                          "规范化字段",
                          "响应原值",
                          "规则期望值",
                          "分区实际值",
                        ].map(label => (
                          <th key={label}>{t(label)}</th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {shown.map((issue, i) => (
                        <tr key={i}>
                          <td>{t(kinds[issue.kind] ?? "对应关系未知的键")}</td>
                          <td>{issue.key}</td>
                          <td>{issue.field}</td>
                          <td>{cell(issue.raw)}</td>
                          <td>{cell(issue.expected)}</td>
                          <td>{cell(issue.actual)}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              </>
            )}
          </>
        )}
        <details>
          <summary>{t("本期字段与单位规则")}</summary>
          <table aria-label={t("本期字段与单位规则")}>
            <thead>
              <tr>
                {["来源字段", "规范化字段", "核对规则"].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {fields.map(([raw, target, rule]) => (
                <tr key={raw}>
                  <td>{raw}</td>
                  <td>{target}</td>
                  <td>{t(rule)}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <p className="subtle">
            {t(
              "14个源字段对应13个分区列及分区日期；空值保持缺失，科学计数法和末尾零按精确数值比较。非日期合约排除与未知键均单列，不能当成零差异覆盖。",
            )}
          </p>
        </details>
      </section>
    </details>
  );
}
