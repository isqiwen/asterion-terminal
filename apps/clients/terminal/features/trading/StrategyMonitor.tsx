import { useState } from "react";
import { Side } from "../../src/generated/asterion/common_pb";
import {
  InstanceState,
  type InstanceStatus,
  type Position,
} from "../../src/generated/asterion/trading_pb";
import { decimalText } from "../../src/bridge/decimal";
import { getLocale, listSeparator } from "../../src/i18n";
import { BackendError, ErrorNotice } from "../../src/i18n/errors";
import { key, t } from "./live-shared";
import { activeStrategy, parameterText, periodText } from "./strategy-instance";

const states: Partial<Record<InstanceState, string>> = {
  [InstanceState.STARTING]: "策略正在启动",
  [InstanceState.WARMING]: "正在现场预热",
  [InstanceState.READY]: "策略已就绪",
  [InstanceState.STOPPED]: "已停止",
  [InstanceState.INTERRUPTED]: "已中断",
  [InstanceState.FAILED]: "策略运行失败",
};

// Long minus short lots of a contract, today's and earlier ones together.
function held(positions: readonly Position[], contract: string) {
  let lots = 0;
  for (const position of positions)
    if (key(position.instrument) === contract)
      lots +=
        (position.side === Side.BUY ? 1 : -1) *
        (Number(decimalText(position.todayLots)) + Number(decimalText(position.yesterdayLots)));
  return String(lots);
}

export function StrategyMonitor({
  instance,
  positions,
  observedAt,
  fresh,
  stopping,
  busy,
  stop,
  pause,
  flatten,
}: {
  instance?: InstanceStatus;
  // The account's positions, to set beside each target.
  positions: readonly Position[];
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
  return (
    <section className="strategy-monitor" aria-label={t("策略运行状态")}>
      <h4>
        {t("策略实例")} · {configuration.id}
      </h4>
      <p role="status">
        {t(states[instance.state] ?? "未知策略状态")}
        {!fresh && ` · ${t("上次已知状态")}`}
      </p>
      {instance.paused && <p role="status">{t("已暂停新增：目标只减仓。")}</p>}
      {instance.error && (
        <p className="alert" role="alert">
          <ErrorNotice error={new BackendError("operation_failed", instance.error)} />
        </p>
      )}
      <dl>
        <dt>{t("已发布版本")}</dt>
        <dd>
          {configuration.packageName} · {configuration.packageVersion}
        </dd>
        <dt>{t("实际合约")}</dt>
        <dd>{configuration.instruments.map(key).join(listSeparator())}</dd>
        <dt>{t("K 线周期")}</dt>
        <dd>{periodText(configuration.period)}</dd>
        <dt>{t("已处理完整 K 线批次")}</dt>
        <dd>{instance.completedSteps.toString()}</dd>
        <dt>{t("最近读取时间")}</dt>
        <dd>{observedAt?.toLocaleTimeString(getLocale(), { hour12: false }) ?? "—"}</dd>
        <dt>{t("参数覆盖")}</dt>
        <dd>
          {Object.entries(configuration.params)
            .map(([name, value]) => `${name} = ${parameterText(value)}`)
            .join(listSeparator()) || t("使用包内默认参数")}
        </dd>
        {configuration.weightLimits && (
          <>
            <dt>{t("总绝对权重预算")}</dt>
            <dd>{decimalText(configuration.weightLimits.gross)}</dd>
            <dt>{t("净权重绝对值预算")}</dt>
            <dd>{decimalText(configuration.weightLimits.net)}</dd>
          </>
        )}
      </dl>
      {instance.state === InstanceState.WARMING && (
        <p className="subtle">
          {t("预热是否完成由策略按参数和实际收到的数据判断；批次数不是预热百分比。")}
        </p>
      )}
      <table aria-label={t("目标与实际持仓")}>
        <thead>
          <tr>
            <th>{t("实际合约")}</th>
            <th className="numeric">
              {instance.lastDecision
                ? t("目标（批次 {n}）", { n: instance.lastDecision.sequence.toString() })
                : t("目标")}
            </th>
            <th className="numeric">{t("实际净持仓")}</th>
          </tr>
        </thead>
        <tbody>
          {configuration.instruments.map(instrument => {
            const target = instance.lastDecision?.targets.find(
              item => key(item.instrument) === key(instrument),
            );
            return (
              <tr key={key(instrument)}>
                <td>{key(instrument)}</td>
                <td className="numeric">
                  {target?.size.case === "lots"
                    ? target.size.value.toString()
                    : target?.size.case === "weight"
                      ? `${decimalText(target.size.value)} ${t("权重")}`
                      : "—"}
                </td>
                <td className="numeric">{held(positions, key(instrument))}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
      {instance.events.length > 0 && (
        <div>
          <h4>{t("最近事件")}</h4>
          <ul className="strategy-events">
            {[...instance.events].reverse().map(event => (
              <li key={`${event.timeMs}-${event.text}`}>
                {new Date(Number(event.timeMs)).toLocaleTimeString(getLocale(), { hour12: false })}{" "}
                · {event.text}
              </li>
            ))}
          </ul>
        </div>
      )}
      <p className="subtle">
        {t("关闭 Terminal 不影响已授权交易。停止策略会尝试撤销其未终结委托，保留持仓与账户授权。")}
      </p>
      {confirm ? (
        <div className="creation-review">
          <p>
            {confirm.action === "stop"
              ? t("确认停止实例 {id} 并尝试撤单？此操作不会平仓。", { id: confirm.id })
              : t("确认对实例 {id} 的合约平仓？实例会暂停新增并继续运行。", { id: confirm.id })}
          </p>
          <button disabled={stopping || busy} onClick={() => setConfirm(undefined)}>
            {t("取消")}
          </button>
          <button
            disabled={stopping || busy}
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
              disabled={busy || stopping}
              onClick={() => setConfirm({ id: configuration.id, action: "flatten" })}
            >
              {t("平仓")}
            </button>
            <button
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
