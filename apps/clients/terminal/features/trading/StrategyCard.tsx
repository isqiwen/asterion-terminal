import { useLayoutEffect, useRef, useState } from "react";
import {
  InstanceState,
  type InstanceStatus,
  type Session,
} from "../../src/generated/asterion/trading_pb";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { t } from "./live-shared";
import { instanceStates } from "./StrategyMonitor";
import { eventTone, targetRows } from "./strategy-instance";

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

// The account's running strategy in the workbench side rail: state, heartbeat,
// targets vs actuals with drift, direct interventions, and recent events.
export function StrategyCard({
  instance,
  live,
  open,
  busy = false,
  stopping = false,
  pause,
  flatten,
  stop,
}: {
  instance: InstanceStatus;
  live: Session;
  open: () => void;
  busy?: boolean;
  stopping?: boolean;
  pause?: (id: string, paused: boolean) => Promise<boolean>;
  flatten?: (id: string) => Promise<boolean>;
  stop?: (id: string) => Promise<boolean>;
}) {
  const configuration = instance.instance!;
  const [confirm, setConfirm] = useState<"stop" | "flatten">();
  const confirmDialog = useRef<HTMLDialogElement>(null);
  useLayoutEffect(() => {
    if (!confirm) return;
    const element = confirmDialog.current;
    element?.showModal();
    return () => element?.close();
  }, [confirm]);
  const shortName = configuration.packageName.split(".").at(-1) || configuration.packageName;

  return (
    <section className="strategy-card" aria-label={t("运行中的策略")}>
      <header className="strategy-card-header">
        <div className="strategy-card-title">
          <strong title={configuration.packageName}>{shortName}</strong>
          <StatusBadge tone={tones[instance.state] ?? "neutral"}>
            {t(instanceStates[instance.state] ?? "未知策略状态")}
          </StatusBadge>
          {instance.paused && <StatusBadge tone="attention">{t("暂停新增")}</StatusBadge>}
        </div>
        <button type="button" className="link" onClick={open}>
          {t("查看策略")}
        </button>
      </header>
      <div className="strategy-card-meta">
        <span>
          {t("实例")} {configuration.id}
        </span>
        <span>
          {t("最近 K 线")} {instance.lastBarMs ? time(instance.lastBarMs) : "—"}
        </span>
        <span>
          {t("批次")} {instance.completedSteps.toString()}
        </span>
      </div>
      {(pause || flatten || stop) && (
        <div className="strategy-actions strategy-card-actions">
          {pause && (
            <button
              type="button"
              disabled={busy || stopping}
              onClick={() => void pause(configuration.id, !instance.paused)}
            >
              {t(instance.paused ? "恢复新增" : "暂停新增")}
            </button>
          )}
          {flatten && (
            <button
              type="button"
              className="danger-quiet"
              disabled={busy || stopping}
              onClick={() => setConfirm("flatten")}
            >
              {t("平仓")}
            </button>
          )}
          {stop && (
            <button
              type="button"
              className="danger-quiet"
              disabled={stopping}
              onClick={() => setConfirm("stop")}
            >
              {t("停止策略并撤单")}
            </button>
          )}
        </div>
      )}
      <div
        className="strategy-card-table"
        tabIndex={0}
        role="region"
        aria-label={t("目标与实际持仓")}
      >
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
              <th scope="col" className="numeric">
                {t("偏差")}
              </th>
            </tr>
          </thead>
          <tbody>
            {targetRows(instance, live.positions).map(row => (
              <tr key={row.label} data-off={row.off || undefined}>
                <td>{row.label}</td>
                <td className="numeric">{row.target}</td>
                <td className="numeric">{row.net}</td>
                <td className="numeric strategy-delta">{row.delta}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {instance.events.length > 0 && (
        <ul className="strategy-events" aria-label={t("最近事件")}>
          {instance.events
            .slice(-3)
            .reverse()
            .map(event => (
              <li key={`${event.timeMs}-${event.text}`} data-tone={eventTone(event.kind)}>
                {new Date(Number(event.timeMs)).toLocaleTimeString("sv-SE", {
                  timeZone: "Asia/Shanghai",
                  hour12: false,
                })}{" "}
                · {event.text}
              </li>
            ))}
        </ul>
      )}
      <p className="subtle strategy-card-note">
        {t("策略运行期间暂停手动开仓；撤单、平仓与停止下单始终可用。")}
      </p>
      {confirm && (
        <dialog
          ref={confirmDialog}
          className="order-confirm"
          role="alertdialog"
          aria-label={t("确认操作")}
          onCancel={event => {
            event.preventDefault();
            if (!stopping) setConfirm(undefined);
          }}
          onClose={event => {
            if (!event.currentTarget.open && !stopping) setConfirm(undefined);
          }}
        >
          <p>
            {confirm === "stop"
              ? t("停止实例 {id} 并撤销它的挂单？持仓保留。", { id: configuration.id })
              : t("平掉实例 {id} 的持仓？实例暂停新增，继续运行。", { id: configuration.id })}
          </p>
          <button
            type="button"
            className="danger"
            disabled={stopping || (confirm !== "stop" && busy)}
            onClick={() => {
              const action =
                confirm === "stop" ? stop?.(configuration.id) : flatten?.(configuration.id);
              void action?.then(done => {
                if (done) setConfirm(undefined);
              });
            }}
          >
            {t(confirm === "stop" ? "确认停止并撤单" : "确认平仓")}
          </button>
          <button type="button" autoFocus disabled={stopping} onClick={() => setConfirm(undefined)}>
            {t("取消")}
          </button>
        </dialog>
      )}
    </section>
  );
}
