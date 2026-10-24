import { useState } from "react";
import { timestamp, type FactorObservation } from "../../src/bridge/client";
import { t, variableUnit } from "./shared";

export function FactorObservations({
  rows,
  kind,
  pricePower,
}: {
  rows: FactorObservation[];
  kind: "bars" | "tables";
  pricePower?: number;
}) {
  const [page, setPage] = useState(0);
  const size = 50;
  const last = Math.max(0, Math.ceil(rows.length / size) - 1);
  const current = Math.min(page, last);
  const begin = current * size;
  const when = (value: string) => (kind === "tables" ? value : timestamp(value));
  return (
    <details>
      <summary>{t("逐行特征与标签证据")}</summary>
      <section aria-label={t("逐行特征与标签证据")}>
        <p>
          {t(
            "明细来自本次固定输入和原计算；未来收益仅用于事后评价，不进入同期特征。显示实际观察序列，不补交易日或推断历史可得时间。",
          )}
        </p>
        <p>
          {t("{start}–{end} / {total} 条实际观察", {
            start: rows.length ? begin + 1 : 0,
            end: Math.min(begin + size, rows.length),
            total: rows.length,
          })}
        </p>
        <div className="research-table">
          <table aria-label={t("逐行特征与标签证据")}>
            <thead>
              <tr>
                {[
                  "观察序号",
                  "原观察时间/交易日",
                  "因子值（比例）",
                  "标签结束观察",
                  "未来收益（比例）",
                  "评价资格",
                  "样本角色",
                ].map(label => (
                  <th key={label}>
                    {label === "因子值（比例）" && pricePower !== undefined
                      ? t("特征值（{unit}）", { unit: variableUnit(pricePower) })
                      : t(label)}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {rows.slice(begin, begin + size).map(row => (
                <tr key={row.event_index}>
                  <td>{row.event_index + 1}</td>
                  <td>{when(row.observed)}</td>
                  <td>
                    {row.value === null
                      ? t(
                          row.feature_state === "FEATURE_ZERO_DENOMINATOR"
                            ? "分母为零，特征无效"
                            : row.feature_state === "FEATURE_NON_FINITE"
                              ? "非有限结果，特征无效"
                              : row.feature_state === "FEATURE_WARMUP"
                                ? "特征预热未完成"
                                : "特征尚未定义",
                        )
                      : String(row.value)}
                  </td>
                  <td>
                    {row.label === null || row.label_end_index === null
                      ? t("未来标签尚未知")
                      : `${row.label_end_index + 1} · ${when(row.label)}`}
                  </td>
                  <td>
                    {row.forward_return === null ? t("未来标签尚未知") : String(row.forward_return)}
                  </td>
                  <td>
                    {t(
                      row.evaluation_warmup
                        ? "评价预热未完成"
                        : row.label_crosses_boundary
                          ? "标签跨开发边界，排除评价"
                          : row.feature_state === "FEATURE_ZERO_DENOMINATOR" ||
                              row.feature_state === "FEATURE_NON_FINITE"
                            ? "特征无效，排除评价"
                            : row.included
                              ? "参与评价"
                              : "未来标签尚未知",
                    )}
                  </td>
                  <td>
                    {t(
                      row.role === "full_sample"
                        ? "全样本"
                        : row.role === "development"
                          ? "开发段"
                          : "留出段",
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <button disabled={current === 0} onClick={() => setPage(current - 1)}>
          {t("上一页逐行证据")}
        </button>
        <button disabled={current === last} onClick={() => setPage(current + 1)}>
          {t("下一页逐行证据")}
        </button>
      </section>
    </details>
  );
}
