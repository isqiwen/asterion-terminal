import { useEffect, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  marketData,
  translate,
  type DisplayError,
  type MessageValues,
  type MinuteComparisonResult,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);

// The contract-days whose minutes both Tushare and the recorder hold, and
// where they disagree. Research takes Tushare's on those days.
export function MinuteComparison() {
  const [retry, setRetry] = useState(0);
  const [state, setState] = useState<{ result?: MinuteComparisonResult; error: DisplayError }>();
  useEffect(() => {
    let active = true;
    void marketData({ case: "minuteComparison", value: {} }, "minuteComparison")
      .then(result => active && setState({ result, error: "" }))
      .catch(reason => active && setState({ error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, [retry]);
  const result = state?.result;
  return (
    <section aria-label={t("Tushare 与自录分钟线对比")}>
      <h3>{t("Tushare 与自录分钟线对比")}</h3>
      {state?.error && <ErrorNotice error={state.error} />}
      {result && (
        <p>
          {result.days
            ? t("两个来源都有的合约交易日 {days} 个，其中 {differing} 个不一致。", {
                days: result.days,
                differing: result.differingDays,
              })
            : t("还没有两个来源都有分钟线的合约交易日。")}
        </p>
      )}
      {!!result?.differing.length && (
        <table aria-label={t("分钟线不一致的合约交易日")}>
          <thead>
            <tr>
              {["合约", "交易日", "对比分钟", "不一致", "仅 Tushare", "仅自录", "自录不完整"].map(
                heading => (
                  <th key={heading}>{t(heading)}</th>
                ),
              )}
            </tr>
          </thead>
          <tbody>
            {result.differing.map(day => (
              <tr key={`${day.contract}:${day.day}`}>
                <td>{day.contract}</td>
                <td>{day.day}</td>
                <td>{day.compared}</td>
                <td>{day.differing}</td>
                <td>{day.tushareOnly}</td>
                <td>{day.recorderOnly}</td>
                <td>{day.partial}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      <p className="subtle">
        {t("逐分钟比较开高低收和成交量；自录中标为不完整的分钟只计数，不比较。")}
      </p>
      <button onClick={() => setRetry(value => value + 1)}>{t("重新对比")}</button>
    </section>
  );
}
