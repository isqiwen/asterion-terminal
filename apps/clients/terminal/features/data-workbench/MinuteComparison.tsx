import { useEffect, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  getLocale,
  marketData,
  translate,
  type DisplayError,
  type MessageValues,
  type MinuteComparisonResult,
  type MinuteDifferences,
  type MinuteValues,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const fields = ["open", "high", "low", "close", "volume"] as const;
const labels = { open: "开", high: "高", low: "低", close: "收", volume: "量" };

// The contract-days whose minutes Tushare and the recorder hold and where
// they disagree; research takes Tushare's on those days. Nothing is shown
// while they agree, or while the comparison cannot be read.
export function MinuteComparison() {
  const [result, setResult] = useState<MinuteComparisonResult>();
  const [retry, setRetry] = useState(0);
  const [open, setOpen] = useState("");
  useEffect(() => {
    let active = true;
    void marketData({ case: "minuteComparison", value: {} }, "minuteComparison")
      .then(next => active && setResult(next))
      .catch(() => active && setResult(undefined));
    return () => {
      active = false;
    };
  }, [retry]);
  if (!result?.differingDays) return null;
  return (
    <section aria-label={t("Tushare 与自录分钟线对比")}>
      <h3>{t("Tushare 与自录分钟线对比")}</h3>
      <p>
        {t("两个来源都有的合约交易日 {days} 个，其中 {differing} 个不一致。", {
          days: result.days,
          differing: result.differingDays,
        })}
      </p>
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
          {result.differing.map(day => {
            const key = `${day.contract}:${day.day}`;
            return (
              <tr
                key={key}
                aria-expanded={open === key}
                onClick={() => setOpen(open === key ? "" : key)}
              >
                <td>{day.contract}</td>
                <td>{day.day}</td>
                <td>{day.compared}</td>
                <td>{day.differing}</td>
                <td>{day.tushareOnly}</td>
                <td>{day.recorderOnly}</td>
                <td>{day.partial}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
      {open && <Minutes key={open} contract={open.split(":")[0]!} day={open.split(":")[1]!} />}
      <p className="subtle">
        {t("逐分钟比较开高低收和成交量；自录中标为不完整的分钟只计数，不比较。")}
      </p>
      <button onClick={() => setRetry(value => value + 1)}>{t("重新对比")}</button>
    </section>
  );
}

// The minutes of one contract-day that differ, with both sources' values.
function Minutes({ contract, day }: { contract: string; day: string }) {
  const [state, setState] = useState<{ result?: MinuteDifferences; error: DisplayError }>();
  useEffect(() => {
    let active = true;
    void marketData({ case: "minuteDifferences", value: { contract, day } }, "minuteDifferences")
      .then(result => active && setState({ result, error: "" }))
      .catch(reason => active && setState({ error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, [contract, day]);
  if (state?.error) return <ErrorNotice error={state.error} />;
  if (!state?.result) return <p role="status">{t("读取中")}</p>;
  const cells = (side: MinuteValues | undefined, other: MinuteValues | undefined) =>
    fields.map(field => (
      <td key={field} data-differs={!side || !other || side[field] !== other[field] || undefined}>
        {side ? side[field] : "—"}
      </td>
    ));
  return (
    <table aria-label={t("{contract} {day} 不一致的分钟", { contract, day })}>
      <thead>
        <tr>
          <th rowSpan={2}>{t("时间")}</th>
          <th colSpan={5}>Tushare</th>
          <th colSpan={5}>{t("自录")}</th>
        </tr>
        <tr>
          {[...fields, ...fields].map((field, index) => (
            <th key={index}>{t(labels[field])}</th>
          ))}
        </tr>
      </thead>
      <tbody>
        {state.result.minutes.map(minute => (
          <tr key={String(minute.startMs)}>
            <td>
              {new Date(Number(minute.startMs)).toLocaleTimeString(getLocale(), {
                timeZone: "Asia/Shanghai",
                hour12: false,
                hour: "2-digit",
                minute: "2-digit",
              })}
            </td>
            {cells(minute.tushare, minute.recorder)}
            {cells(minute.recorder, minute.tushare)}
          </tr>
        ))}
      </tbody>
    </table>
  );
}
