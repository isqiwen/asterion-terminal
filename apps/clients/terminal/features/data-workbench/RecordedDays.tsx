import { useEffect, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  getLocale,
  marketData,
  translate,
  type DisplayError,
  type MessageValues,
} from "../contract";
import type { RecordedDay } from "../../src/generated/asterion/market_pb";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const time = (ms: bigint) =>
  new Date(Number(ms)).toLocaleTimeString(getLocale(), {
    timeZone: "Asia/Shanghai",
    hour12: false,
  });
const minutes = (ms: bigint) => Math.round(Number(ms) / 60000);
const thisMonth = () => new Date().toISOString().slice(0, 7);
// The time between one segment's last quote and the next one's first.
const gaps = (day: RecordedDay) =>
  day.segments
    .slice(1)
    .map((segment, i) => segment.firstReceivedMs - day.segments[i]!.lastReceivedMs);

// The recorder's observation per trading day of a month: each stretch it
// watched the market, why it stopped and the gaps between.
export function RecordedDays() {
  const [month, setMonth] = useState(thisMonth);
  const [days, setDays] = useState<RecordedDay[]>([]);
  const [open, setOpen] = useState("");
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    const [year, number] = month.split("-").map(Number);
    const last = new Date(Date.UTC(year!, number!, 0)).toISOString().slice(0, 10);
    void marketData({
      case: "listRecordedDays",
      value: { firstDay: `${month}-01`, lastDay: last },
    })
      .then(result => {
        if (!active) return;
        setDays(result.days);
        setError("");
      })
      .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [month]);
  const shown = days.find(day => day.tradingDay === open);
  return (
    <section className="data-card" aria-label={t("自录观测")}>
      <header>
        <h3>{t("自录观测")}</h3>
        <input
          type="month"
          aria-label={t("月份")}
          value={month}
          onChange={event => event.target.value && setMonth(event.target.value)}
        />
      </header>
      <ErrorNotice error={error} />
      {days.length ? (
        <table className="data-table" aria-label={t("自录观测")}>
          <thead>
            <tr>
              <th>{t("交易日")}</th>
              <th className="numeric">{t("观测段")}</th>
              <th className="numeric">{t("Tick 数")}</th>
              <th className="numeric">{t("缺口")}</th>
            </tr>
          </thead>
          <tbody>
            {days.map(day => {
              const missing = gaps(day);
              return (
                <tr
                  key={day.tradingDay}
                  aria-expanded={open === day.tradingDay}
                  onClick={() => setOpen(open === day.tradingDay ? "" : day.tradingDay)}
                >
                  <td>{day.tradingDay}</td>
                  <td className="numeric">{day.segments.length}</td>
                  <td className="numeric">
                    {day.segments
                      .reduce((sum, segment) => sum + segment.ticks, 0n)
                      .toLocaleString(getLocale())}
                  </td>
                  <td className="numeric">
                    {missing.length
                      ? t("{count} 处，共 {minutes} 分钟", {
                          count: missing.length,
                          minutes: minutes(missing.reduce((sum, gap) => sum + gap, 0n)),
                        })
                      : "—"}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      ) : (
        !error && <p className="subtle">{t("这个月没有录制。")}</p>
      )}
      {shown && (
        <table className="data-table" aria-label={t("{day} 的观测段", { day: shown.tradingDay })}>
          <thead>
            <tr>
              <th>{t("开始")}</th>
              <th>{t("结束")}</th>
              <th className="numeric">{t("Tick 数")}</th>
              <th>{t("结束原因")}</th>
              <th className="numeric">{t("之前的缺口")}</th>
            </tr>
          </thead>
          <tbody>
            {shown.segments.map((segment, index) => (
              <tr key={`${segment.instance}/${segment.number}`}>
                <td>{time(segment.firstReceivedMs)}</td>
                <td>{time(segment.lastReceivedMs)}</td>
                <td className="numeric">{segment.ticks.toLocaleString(getLocale())}</td>
                <td>{segment.end || t("进程停止前未记录原因")}</td>
                <td className="numeric">
                  {index ? t("{minutes} 分钟", { minutes: minutes(gaps(shown)[index - 1]!) }) : "—"}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </section>
  );
}
