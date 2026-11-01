import { useState } from "react";
import type { InstanceStatus } from "../../src/generated/asterion/strategy_pb";
import { BackendError, ErrorNotice, getLocale } from "../contract";
import { t } from "./live-shared";
import { activeStrategy, parameterText, weightText } from "./strategy-instance";

const states: Record<string, string> = {
  starting: "策略正在启动",
  warming: "正在现场预热",
  ready: "策略已就绪",
  stopped: "已停止",
  interrupted: "已中断",
  failed: "策略运行失败",
};

export function StrategyMonitor({
  instance,
  observedAt,
  fresh,
  stopping,
  stop,
}: {
  instance?: InstanceStatus;
  observedAt?: Date;
  fresh: boolean;
  stopping: boolean;
  stop: (id: string) => Promise<boolean>;
}) {
  const [confirm, setConfirm] = useState<string>();
  const configuration = instance?.configuration;
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
      {instance.error && (
        <p className="alert" role="alert">
          <ErrorNotice error={new BackendError("operation_failed", instance.error)} />
        </p>
      )}
      <dl>
        <dt>{t("已发布版本")}</dt>
        <dd>
          {configuration.package?.name} · {configuration.package?.version}
        </dd>
        <dt>{t("实际合约")}</dt>
        <dd>{configuration.contracts.join(", ")}</dd>
        <dt>{t("K 线周期")}</dt>
        <dd>{configuration.period}</dd>
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
        {configuration.weightBudget && (
          <>
            <dt>{t("总绝对权重预算")}</dt>
            <dd>{weightText(configuration.weightBudget.grossUnits!)}</dd>
            <dt>{t("净权重绝对值预算")}</dt>
            <dd>{weightText(configuration.weightBudget.netUnits!)}</dd>
          </>
        )}
      </dl>
      {instance.state === "warming" && (
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
                  <tr key={target.contract}>
                    <td>{target.contract}</td>
                    <td>{t(target.value.case === "lots" ? "手数" : "权重")}</td>
                    <td>
                      {target.value.case === "lots"
                        ? target.value.value.toString()
                        : target.value.case === "weightUnits"
                          ? weightText(target.value.value)
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
      <p className="subtle">
        {t("关闭 Terminal 不影响已授权交易。停止策略会尝试撤销其未终结委托，保留持仓与账户授权。")}
      </p>
      {confirm ? (
        <div className="creation-review">
          <p>{t("确认停止实例 {id} 并尝试撤单？此操作不会平仓。", { id: confirm })}</p>
          <button disabled={stopping} onClick={() => setConfirm(undefined)}>
            {t("取消")}
          </button>
          <button
            disabled={stopping}
            onClick={() =>
              void stop(confirm).then(stopped => {
                if (stopped) setConfirm(undefined);
              })
            }
          >
            {t("确认停止并撤单")}
          </button>
        </div>
      ) : (
        activeStrategy(instance.state) && (
          <button disabled={stopping} onClick={() => setConfirm(configuration.id)}>
            {t("停止策略并撤单")}
          </button>
        )
      )}
    </section>
  );
}
