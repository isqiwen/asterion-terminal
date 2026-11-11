import { useState } from "react";
import { InstanceState, type InstanceStatus } from "../../src/generated/asterion/trading_pb";
import { decimalText } from "../../src/bridge/decimal";
import { getLocale } from "../../src/i18n";
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

export function StrategyMonitor({
  instance,
  observedAt,
  fresh,
  stopping,
  busy,
  stop,
  pause,
  flatten,
}: {
  instance?: InstanceStatus;
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
        <dd>{configuration.instruments.map(key).join(", ")}</dd>
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
            .join(", ") || t("使用包内默认参数")}
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
      {instance.lastDecision && (
        <div>
          <h4>{t("最近目标批次 {n}", { n: instance.lastDecision.sequence.toString() })}</h4>
          {instance.lastDecision.targets.length ? (
            <table>
              <thead>
                <tr>
                  <th>{t("实际合约")}</th>
                  <th>{t("目标类型")}</th>
                  <th>{t("目标值")}</th>
                </tr>
              </thead>
              <tbody>
                {instance.lastDecision.targets.map(target => (
                  <tr key={key(target.instrument)}>
                    <td>{key(target.instrument)}</td>
                    <td>{t(target.size.case === "lots" ? "手数" : "权重")}</td>
                    <td>
                      {target.size.case === "lots"
                        ? target.size.value.toString()
                        : target.size.case === "weight"
                          ? decimalText(target.size.value)
                          : "—"}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : (
            <p>{t("本批没有可执行目标")}</p>
          )}
          <p className="subtle">{t("目标不代表成交；实际持仓和委托请查看下方账户记录。")}</p>
        </div>
      )}
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
