import {
  InstanceState,
  type InstanceStatus,
  type Session,
} from "../../src/generated/asterion/trading_pb";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { t } from "./live-shared";
import { instanceStates } from "./StrategyMonitor";
import { targetRows } from "./strategy-instance";

const tones: Partial<Record<InstanceState, Tone>> = {
  [InstanceState.STARTING]: "attention",
  [InstanceState.WARMING]: "attention",
  [InstanceState.READY]: "ok",
};
// Beijing month, day and minute, as MM-DD HH:mm.
const time = (ms: bigint) =>
  new Date(Number(ms))
    .toLocaleString("sv-SE", { timeZone: "Asia/Shanghai", hour12: false })
    .slice(5, 16);

// The account's running strategy at a glance: its state, the latest bars it
// decided on, its targets against the positions, and its latest events.
export function StrategyCard({
  instance,
  live,
  open,
}: {
  instance: InstanceStatus;
  live: Session;
  open: () => void;
}) {
  const configuration = instance.instance!;
  return (
    <section className="strategy-card" aria-label={t("运行中的策略")}>
      <header>
        <strong>{configuration.packageName}</strong>
        <StatusBadge tone={tones[instance.state] ?? "neutral"}>
          {t(instanceStates[instance.state] ?? "未知策略状态")}
        </StatusBadge>
        {instance.paused && <StatusBadge tone="attention">{t("暂停新增")}</StatusBadge>}
        <span>
          {t("最近 K 线")} {instance.lastBarMs ? time(instance.lastBarMs) : "—"}
        </span>
        <button type="button" className="link" onClick={open}>
          {t("查看策略")}
        </button>
      </header>
      <table aria-label={t("目标与实际持仓")}>
        <thead>
          <tr>
            <th scope="col">{t("实际合约")}</th>
            <th scope="col" className="numeric">
              {t("目标")}
            </th>
            <th scope="col" className="numeric">
              {t("实际净持仓")}
            </th>
          </tr>
        </thead>
        <tbody>
          {targetRows(instance, live.positions).map(row => (
            <tr
              key={row.label}
              data-off={(/^-?\d+$/.test(row.target) && row.target !== row.net) || undefined}
            >
              <td>{row.label}</td>
              <td className="numeric">{row.target}</td>
              <td className="numeric">{row.net}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {instance.events.length > 0 && (
        <ul className="strategy-events" aria-label={t("最近事件")}>
          {instance.events
            .slice(-3)
            .reverse()
            .map(event => (
              <li key={`${event.timeMs}-${event.text}`}>
                {new Date(Number(event.timeMs)).toLocaleTimeString("sv-SE", {
                  timeZone: "Asia/Shanghai",
                  hour12: false,
                })}{" "}
                · {event.text}
              </li>
            ))}
        </ul>
      )}
    </section>
  );
}
