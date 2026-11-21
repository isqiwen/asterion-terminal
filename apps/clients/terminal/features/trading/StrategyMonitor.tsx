import { useState } from "react";
import {
  BrokerState,
  InstanceState,
  type InstanceStatus,
} from "../../src/generated/asterion/trading_pb";
import type { TradingView } from "../../src/host/trading-accounts";
import { decimalText } from "../../src/bridge/decimal";
import { listSeparator } from "../../src/i18n";
import { BackendError, ErrorNotice } from "../../src/i18n/errors";
import { key, t } from "./live-shared";
import { activeStrategy, parameterText, periodText, targetRows } from "./strategy-instance";

export const instanceStates: Partial<Record<InstanceState, string>> = {
  [InstanceState.STARTING]: "策略正在启动",
  [InstanceState.WARMING]: "正在现场预热",
  [InstanceState.READY]: "策略已就绪",
  [InstanceState.STOPPED]: "已停止",
  [InstanceState.INTERRUPTED]: "已中断",
  [InstanceState.FAILED]: "策略运行失败",
};

export function StrategyMonitor({
  instance,
  account,
  observedAt,
  fresh,
  stopping,
  busy,
  stop,
  pause,
  flatten,
}: {
  instance?: InstanceStatus;
  account?: TradingView;
  observedAt?: Date;
  fresh: boolean;
  stopping: boolean;
  busy: boolean;
  stop: (id: string) => Promise<boolean>;
  pause: (id: string, paused: boolean) => Promise<boolean>;
  flatten: (id: string) => Promise<boolean>;
}) {
  const [confirm, setConfirm] = useState<{ id: string; action: "stop" | "flatten" }>();
  const configuration = instance?.instance;
  if (!instance || !configuration) return null;
  const positions = account?.positions;
  const positionState = account?.failed
    ? t("服务失联")
    : account?.live && account.live.brokerState !== BrokerState.READY
      ? t("柜台未就绪")
      : account?.live && (!account.live.positionsReconciled || account.live.recoveryRequired)
        ? t("持仓待核对")
        : "";
  return (
    <section className="strategy-monitor" aria-label={t("策略运行状态")}>
      <h2>
        {t("策略实例")} · {configuration.id}
      </h2>
      <p role="status">
        {t(instanceStates[instance.state] ?? "未知策略状态")}
        {!fresh && ` · ${t("上次已知状态")}`}
      </p>
      {instance.paused && <p role="status">{t("已暂停新增：目标只减仓。")}</p>}
      {instance.error && (
        <p className="alert" role="alert">
          <ErrorNotice error={new BackendError("operation_failed", instance.error)} />
        </p>
      )}
      <dl>
        <dt>{t("已部署版本")}</dt>
        <dd>
          {configuration.packageName} · {configuration.packageVersion}
        </dd>
        <dt>{t("实际合约")}</dt>
        <dd>
          {[
            ...configuration.instruments.map(key),
            ...configuration.series.map(item => `${item.exchange}.${item.product}`),
          ].join(listSeparator())}
        </dd>
        <dt>{t("K 线周期")}</dt>
        <dd>{periodText(configuration.period)}</dd>
        {configuration.warmup?.datasetVersion && (
          <>
            <dt>{t("历史预热")}</dt>
            <dd>
              {configuration.warmup.datasetVersion} · {configuration.warmup.sha256.slice(0, 12)}
            </dd>
          </>
        )}
        <dt>{t("已处理完整 K 线批次")}</dt>
        <dd>{instance.completedSteps.toString()}</dd>
        <dt>{t("策略读取时间")}</dt>
        <dd>
          {observedAt?.toLocaleTimeString("sv-SE", {
            timeZone: "Asia/Shanghai",
            hour12: false,
          }) ?? "—"}
        </dd>
        <dt>{t("持仓读取时间")}</dt>
        <dd>
          {[
            positions
              ? positions.readAt.toLocaleString("sv-SE", {
                  timeZone: "Asia/Shanghai",
                  hour12: false,
                })
              : t("持仓未读取"),
            positionState,
            positions && positionState ? t("上次已核对持仓") : "",
          ]
            .filter(Boolean)
            .join(" · ")}
        </dd>
        <dt>{t("参数覆盖")}</dt>
        <dd>
          {Object.entries(configuration.params)
            .map(([name, value]) => `${name} = ${parameterText(value)}`)
            .join(listSeparator()) || t("使用默认参数")}
        </dd>
        {configuration.weightLimits && (
          <>
            <dt>{t("总绝对权重预算")}</dt>
            <dd>{decimalText(configuration.weightLimits.gross)}</dd>
            <dt>{t("净权重绝对值预算")}</dt>
            <dd>{decimalText(configuration.weightLimits.net)}</dd>
            {configuration.weightLimits.maxMarginRatio?.text && (
              <>
                <dt>{t("保证金率上限")}</dt>
                <dd>{decimalText(configuration.weightLimits.maxMarginRatio)}</dd>
              </>
            )}
            {configuration.weightLimits.minRebalanceWeight?.text && (
              <>
                <dt>{t("最小调仓偏离")}</dt>
                <dd>{decimalText(configuration.weightLimits.minRebalanceWeight)}</dd>
              </>
            )}
          </>
        )}
      </dl>
      <table aria-label={t("目标与实际持仓")}>
        <thead>
          <tr>
            <th scope="col">{t("实际合约")}</th>
            <th scope="col" className="numeric">
              {instance.lastDecision
                ? t("目标（批次 {n}）", { n: instance.lastDecision.sequence.toString() })
                : t("目标")}
            </th>
            <th scope="col" className="numeric">
              {t("实际净持仓")}
            </th>
          </tr>
        </thead>
        <tbody>
          {targetRows(instance, positions?.rows).map(row => (
            <tr key={row.label}>
              <td>{row.label}</td>
              <td className="numeric">{row.target}</td>
              <td className="numeric">{row.net}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {instance.events.length > 0 && (
        <div>
          <h3>{t("最近事件")}</h3>
          <ul className="strategy-events">
            {[...instance.events].reverse().map(event => (
              <li key={`${event.timeMs}-${event.text}`}>
                {new Date(Number(event.timeMs)).toLocaleTimeString("sv-SE", {
                  timeZone: "Asia/Shanghai",
                  hour12: false,
                })}{" "}
                · {event.text}
              </li>
            ))}
          </ul>
        </div>
      )}
      {confirm && confirm.id === configuration.id && activeStrategy(instance.state) ? (
        <div className="creation-review">
          <p>
            {confirm.action === "stop"
              ? t("停止实例 {id} 并撤销它的挂单？持仓保留。", { id: confirm.id })
              : t("平掉实例 {id} 的持仓？实例暂停新增，继续运行。", { id: confirm.id })}
          </p>
          <button disabled={stopping} onClick={() => setConfirm(undefined)}>
            {t("取消")}
          </button>
          <button
            className="danger"
            disabled={stopping || (confirm.action !== "stop" && busy)}
            onClick={() =>
              void (confirm.action === "stop" ? stop(confirm.id) : flatten(confirm.id)).then(
                done => {
                  if (done) setConfirm(undefined);
                },
              )
            }
          >
            {t(confirm.action === "stop" ? "确认停止并撤单" : "确认平仓")}
          </button>
        </div>
      ) : (
        activeStrategy(instance.state) && (
          <div className="strategy-actions">
            <button
              disabled={busy || stopping}
              onClick={() => void pause(configuration.id, !instance.paused)}
            >
              {t(instance.paused ? "恢复新增" : "暂停新增")}
            </button>
            <button
              className="danger-quiet"
              disabled={busy || stopping}
              onClick={() => setConfirm({ id: configuration.id, action: "flatten" })}
            >
              {t("平仓")}
            </button>
            <button
              className="danger-quiet"
              disabled={stopping}
              onClick={() => setConfirm({ id: configuration.id, action: "stop" })}
            >
              {t("停止策略并撤单")}
            </button>
          </div>
        )
      )}
    </section>
  );
}
