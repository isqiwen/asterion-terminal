import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

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
