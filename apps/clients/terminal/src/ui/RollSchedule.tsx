import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// The trading days of a dominant series on which its dominant month has no
// bars: it did not trade, or its bars are missing. A month counts while it is
// dominant; the days it is read on beside those, to take over and to be
// closed, are not the series'.
export function daysWithoutBars(
  rolls: { trading_day: string; symbol: string }[],
  months: { symbol: string; uncovered_days: string[] }[],
): string[] {
  return rolls.flatMap((roll, index) => {
    const next = rolls[index + 1]?.trading_day;
    const month = months.find(item => item.symbol === roll.symbol);
    return (month?.uncovered_days ?? []).filter(
      day => day >= roll.trading_day && (next === undefined || day < next),
    );
  });
}

// The months of a dominant series in turn: the day each takes over, and the
// factor its prices are scaled by for signals. Folded away: ten years of a
// product that rolls every month are more than a hundred rows.
export function RollSchedule({
  name,
  rolls,
}: {
  name: string;
  rolls: { trading_day: string; symbol: string; factor: string }[];
}) {
  return (
    <details>
      <summary>{t("换月日程（{n} 次换月）", { n: rolls.length - 1 })}</summary>
      <table className="data-table" aria-label={t("{name} 换月日程", { name })}>
        <thead>
          <tr>
            <th>{t("起始交易日")}</th>
            <th>{t("主力合约")}</th>
            <th>{t("复权系数")}</th>
          </tr>
        </thead>
        <tbody>
          {rolls.map(roll => (
            <tr key={roll.trading_day}>
              <td>{roll.trading_day}</td>
              <td>{roll.symbol}</td>
              <td>{roll.factor}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </details>
  );
}
