import { translate } from "../contract";
export type Activity = "positions" | "orders" | "fills";
const t = (key: string) => translate("asterion.terminal.trading", key);
export function ActivityTabs({
  value,
  onChange,
}: {
  value: Activity;
  onChange: (value: Activity) => void;
}) {
  const entries: [Activity, string][] = [
    ["positions", "持仓"],
    ["orders", "委托"],
    ["fills", "成交"],
  ];
  return (
    <div className="activity-tabs" role="group" aria-label={t("账户明细")}>
      {entries.map(([key, label]) => (
        <button key={key} aria-pressed={value === key} onClick={() => onChange(key)}>
          {t(label)}
        </button>
      ))}
    </div>
  );
}
