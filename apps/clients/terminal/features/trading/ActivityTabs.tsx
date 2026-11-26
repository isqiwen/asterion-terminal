import { translate } from "../../src/i18n";
export type Activity = "positions" | "orders" | "fills" | "performance" | "rates" | "policy";
const t = (key: string) => translate("asterion.terminal.trading", key);
// `counts` follow a tab's name, such as the working orders of all orders.
export function ActivityTabs({
  value,
  onChange,
  counts,
}: {
  value: Activity;
  onChange: (value: Activity) => void;
  counts: Partial<Record<Activity, string>>;
}) {
  const entries: [Activity, string][] = [
    ["positions", "持仓"],
    ["orders", "委托"],
    ["fills", "成交"],
    ["performance", "绩效"],
    ["rates", "费率"],
    ["policy", "风控"],
  ];
  return (
    <div className="activity-tabs tabs" role="group" aria-label={t("账户明细")}>
      {entries.map(([key, label]) => (
        <button
          key={key}
          type="button"
          aria-pressed={value === key}
          title={counts[key] ? `${t(label)} (${counts[key]})` : undefined}
          onClick={() => onChange(key)}
        >
          {t(label)}
          {counts[key] && <small aria-hidden="true">{counts[key]}</small>}
        </button>
      ))}
    </div>
  );
}
