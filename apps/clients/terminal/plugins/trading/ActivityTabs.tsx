import { translate } from "../contract";
export type Activity = "positions" | "orders" | "fills" | "strategy";
const t = (key: string) => translate("asterion.terminal.trading", key);
export function ActivityTabs({
  value,
  onChange,
  strategy = false,
}: {
  value: Activity;
  onChange: (value: Activity) => void;
  strategy?: boolean;
}) {
  const entries: [Activity, string][] = [
    ["positions", "持仓"],
    ["orders", "委托"],
    ["fills", "成交"],
  ];
  if (strategy) entries.push(["strategy", "策略"]);
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
