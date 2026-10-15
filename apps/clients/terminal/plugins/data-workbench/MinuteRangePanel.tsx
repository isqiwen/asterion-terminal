import { useEffect, useRef, useState } from "react";
import { timestamp, type DailyTable, type MinuteRangeEvidence } from "../../src/bridge/client";
import {
  type TerminalContext,
  translate,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
const states: Record<MinuteRangeEvidence["days"][number]["state"], string> = {
  RANGE_OBSERVED: "已观察到分钟记录",
  RANGE_UNCOVERED: "目标交易日未观察到分钟记录",
  RANGE_CLOSED: "日历标记休市",
  RANGE_UNKNOWN_CALENDAR: "日历未知",
  RANGE_UNKNOWN_DAILY: "日线参考不足",
  RANGE_NO_AUCTION: "日线未报告竞价成交",
  RANGE_UNKNOWN_LABELS: "存在无法归属交易日的记录",
  RANGE_CALENDAR_CONFLICT: "分钟交易日与休市日历冲突",
  RANGE_OFFICIAL_CLOSED: "官方公告明确休市",
  RANGE_RULE_CONFLICT: "本地观测或参考与官方休市公告冲突",
};
// Remounted by the owner for a different service, minute version or reference snapshot.
export function MinuteRangePanel({
  id,
  daily,
  query,
  disabled,
}: Pick<TerminalContext, "query"> & { id: string; daily: DailyTable | null; disabled: boolean }) {
  const [begin, setBegin] = useState("");
  const [end, setEnd] = useState("");
  const [evidence, setEvidence] = useState<MinuteRangeEvidence | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const generation = useRef(0);
  useEffect(
    () => () => {
      ++generation.current;
    },
    [],
  );
  async function load() {
    if (!daily) return;
    const current = ++generation.current;
    setLoading(true);
    setError("");
    setEvidence(null);
    try {
      const result = (
        await query("data.minutes.range", {
          dataset_id: id,
          daily,
          begin_day: begin,
          end_day: end,
          session_rules_version: 1,
        })
      ).minute_range;
      if (current !== generation.current) return;
      if (
        !result ||
        result.session_rules_version !== 1 ||
        result.dataset_id !== id ||
        result.begin_day !== begin ||
        result.end_day !== end ||
        result.daily.snapshot !== daily.snapshot ||
        result.daily.calendar_source !== daily.calendar_source ||
        result.daily.daily_source !== daily.daily_source ||
        result.daily.contracts_source !== daily.contracts_source
      )
        throw new Error(t("返回的数据与所选数据集不匹配"));
      setEvidence(result);
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setLoading(false);
    }
  }
  return (
    <section aria-label={t("日期范围覆盖核对")} aria-busy={loading}>
      <h4>{t("日期范围覆盖核对")}</h4>
      <p className="subtle">
        {t("选择目标交易日日期范围（含首尾，最多366个自然日）。仅核对本地固定版本，不下载数据。")}
      </p>
      <label>
        {t("目标起始交易日")}
        <input
          type="date"
          value={begin}
          disabled={loading}
          onChange={event => {
            setBegin(event.target.value);
            setEvidence(null);
          }}
        />
      </label>
      <label>
        {t("目标结束交易日")}
        <input
          type="date"
          value={end}
          disabled={loading}
          onChange={event => {
            setEnd(event.target.value);
            setEvidence(null);
          }}
        />
      </label>
      <button
        disabled={disabled || loading || !daily || !begin || !end || end < begin}
        onClick={() => void load()}
      >
        {t(loading ? "读取中" : "核对目标范围")}
      </button>
      {!daily && <p>{t("没有可固定的日线和日历参考，请先检查本地日线覆盖。")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {evidence && (
        <>
          <p>
            {t("固定分钟版本")}：<code>{evidence.dataset_id}</code>
          </p>
          <p>
            {t("固定参考快照")}：<code>{evidence.daily.snapshot}</code> ·{" "}
            {evidence.daily.daily_source} · {evidence.daily.calendar_source}
          </p>
          <p>
            {t("目标交易日范围")}：{evidence.begin_day} — {evidence.end_day}
          </p>
          <p>
            {t("原始请求时段（北京时间）")}：{timestamp(evidence.request_begin_ns)} —{" "}
            {timestamp(evidence.request_end_ns)}
          </p>
          <p>
            {t("缺少交易日标记的记录")}：{evidence.unknown_rows}
          </p>
          <p className="subtle">
            {t(
              "目标范围独立于原始下载时段。未观察到记录不等于原始下载缺失；日内、交易时段及夜盘边界完整性均未验证。未知与缺口不补零，也不视为完整。",
            )}
          </p>
          <p className="subtle">
            {t(
              "官方规则仅覆盖已列明日期；本批未核实其他日期的完整时段。停夜盘按晚间自然日列示，不能由开市日历推算节前夜盘。",
            )}
          </p>
          <p className="subtle">
            {t(
              "交易日标签可能由数据源适配器推导；标签与分钟记录本身不能证明夜盘开市。本批未开启日内完整性判定。",
            )}
          </p>
          <table className="data-table">
            <thead>
              <tr>
                {[
                  "交易日",
                  "范围核对结果",
                  "官方规则证据",
                  "实际记录数",
                  "实际首条（北京时间）",
                  "实际末条（北京时间）",
                ].map(key => (
                  <th key={key}>{t(key)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {evidence.days.map(day => (
                <tr key={day.trading_day}>
                  <td>{day.trading_day}</td>
                  <td>{t(states[day.state])}</td>
                  <td>
                    {day.official_closure && (
                      <p>
                        {t("休市公告")}：
                        <a href={day.official_closure.source_url} target="_blank" rel="noreferrer">
                          {day.official_closure.rule_id} v{day.official_closure.version}
                        </a>{" "}
                        · {t("发布日期")} {day.official_closure.published_day}
                      </p>
                    )}
                    {day.evening_closure && (
                      <p>
                        {t("该自然日晚间停夜盘，不推定夜盘交易日归属")}：
                        <a href={day.evening_closure.source_url} target="_blank" rel="noreferrer">
                          {day.evening_closure.rule_id} v{day.evening_closure.version}
                        </a>
                      </p>
                    )}
                    {!day.official_closure && !day.evening_closure && t("本批未核实官方时段规则")}
                  </td>
                  <td>{day.observed?.rows ?? "—"}</td>
                  <td>{day.observed ? timestamp(day.observed.first_ns) : "—"}</td>
                  <td>{day.observed ? timestamp(day.observed.last_ns) : "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}
    </section>
  );
}
