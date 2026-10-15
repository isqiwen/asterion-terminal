import { useEffect, useRef, useState } from "react";
import { timestamp, type MinuteEvidence } from "../../src/bridge/client";
import {
  type TerminalContext,
  translate,
  type MessageValues,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);

// The owner remounts this view when the dataset or data-service identity changes.
export function MinuteEvidencePanel({
  id,
  query,
  disabled,
}: Pick<TerminalContext, "query"> & { id: string; disabled: boolean }) {
  const [evidence, setEvidence] = useState<MinuteEvidence | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const generation = useRef(0);
  useEffect(
    () => () => {
      ++generation.current;
    },
    [],
  );
  async function load(offset: number) {
    const current = ++generation.current;
    setLoading(true);
    setError("");
    try {
      const result = await query("data.minutes.evidence", { dataset_id: id, offset, limit: 50 });
      if (current !== generation.current) return;
      if (!result.minute_evidence || result.minute_evidence.dataset_id !== id)
        throw new Error(t("返回的数据与所选数据集不匹配"));
      setEvidence(result.minute_evidence);
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setLoading(false);
    }
  }
  return (
    <section aria-label={t("交易日记录证据")} aria-busy={loading}>
      <button disabled={disabled || loading} onClick={() => void load(0)}>
        {t(loading ? "读取中" : "按交易日检查")}
      </button>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {evidence && (
        <>
          <p>
            {t("原始请求时段：{begin} — {end}", {
              begin: timestamp(evidence.begin_ns),
              end: timestamp(evidence.end_ns),
            })}
          </p>
          <p>
            {t("共 {rows} 条记录，{days} 个已标记交易日；{unknown} 条记录缺少交易日标记。", {
              rows: evidence.total_rows,
              days: evidence.total_days,
              unknown: evidence.unknown_rows,
            })}
          </p>
          <p className="subtle">
            {t(
              "按数据源给出的交易日归组，夜盘不按自然日拆分。记录数与首末时间仅是观测证据，不能证明日内完整；未判断休市、无成交或盘中断档。",
            )}
          </p>
          {evidence.total_days ? (
            <>
              <table className="data-table">
                <thead>
                  <tr>
                    {[
                      "交易日",
                      "记录数",
                      "实际首条（北京时间）",
                      "实际末条（北京时间）",
                      "日内完整性",
                    ].map(key => (
                      <th key={key}>{t(key)}</th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {evidence.days.map(day => (
                    <tr key={day.trading_day}>
                      <td>{day.trading_day}</td>
                      <td>{day.rows}</td>
                      <td>{timestamp(day.first_ns)}</td>
                      <td>{timestamp(day.last_ns)}</td>
                      <td>{t("未验证")}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
              <div className="history-pagination">
                <span>
                  {evidence.offset + 1}–{evidence.offset + evidence.days.length} /{" "}
                  {evidence.total_days}
                </span>
                <button
                  disabled={disabled || loading || !evidence.offset}
                  onClick={() => void load(Math.max(0, evidence.offset - evidence.limit))}
                >
                  {t("上一页")}
                </button>
                <button
                  disabled={
                    disabled ||
                    loading ||
                    evidence.offset + evidence.days.length >= evidence.total_days
                  }
                  onClick={() => void load(evidence.offset + evidence.limit)}
                >
                  {t("下一页")}
                </button>
              </div>
            </>
          ) : (
            <p>{t("没有可归属到交易日的分钟记录。")}</p>
          )}
        </>
      )}
    </section>
  );
}
