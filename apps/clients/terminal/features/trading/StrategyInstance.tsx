import { useState } from "react";
import { create } from "@bufbuild/protobuf";
import type { LiveSession } from "../../src/bridge/client";
import {
  InstanceSchema,
  type Instance,
  type State,
} from "../../src/generated/asterion/strategy_pb";
import { ErrorNotice, asDisplayError, type DisplayError } from "../contract";
import { t } from "./live-shared";
import {
  activeStrategy,
  parameterText,
  parameterValues,
  weightText,
  weightUnits,
  type ParameterInput,
} from "./strategy-instance";

export type StrategyAccount = Pick<
  LiveSession,
  | "phase"
  | "positions_reconciled"
  | "storage_state"
  | "policy_revision"
  | "contracts"
  | "broker"
  | "environment"
  | "authorization"
>;

export function StrategyInstance({
  state,
  live,
  available,
  busy,
  start,
}: {
  state: State;
  live?: StrategyAccount;
  available: boolean;
  busy: boolean;
  start: (configuration: Instance) => Promise<boolean>;
}) {
  const [id, setId] = useState("");
  const [packageKey, setPackage] = useState("");
  const [selected, setSelected] = useState<string[]>([]);
  const [period, setPeriod] = useState("1m");
  const [rows, setRows] = useState<ParameterInput[]>([]);
  const [budget, setBudget] = useState(false);
  const [gross, setGross] = useState("");
  const [net, setNet] = useState("");
  const [error, setError] = useState<DisplayError>("");
  const [review, setReview] = useState<{ configuration: Instance; policy: string }>();
  const contracts = live?.contracts.map(item => `${item.venue}/${item.symbol}`) ?? [];
  const usable =
    available &&
    live?.phase === "ready" &&
    live.positions_reconciled &&
    live.storage_state !== "recovery_required";
  const running = activeStrategy(state.instance?.state);
  const update = (id: number, change: Partial<ParameterInput>) =>
    setRows(current => current.map(row => (row.id === id ? { ...row, ...change } : row)));

  if (running || !state.packages.length) return null;
  return (
    <section className="strategy-instance" aria-label={t("配置策略实例")}>
      <h4>{t("配置策略实例")}</h4>
      {live && (
        <p>
          {t("CTP 账户")} · {live.broker.broker_id} / {live.broker.user_id} ·{" "}
          {t("主机配置的柜台环境：{environment}", {
            environment: t(live.environment === "real" ? "真实资金" : "CTP 仿真"),
          })}{" "}
          · {t(live.authorization ? "已授权" : "尚未允许发送委托")}
        </p>
      )}
      {!usable && <p role="status">{t("请先连接并同步账户，再启动实例。")}</p>}
      <p className="subtle">
        {t(
          "启动从空窗口现场预热。启动不授权；首次目标执行前须在账户中授权，已授权账户就绪后可能发送委托。",
        )}
      </p>
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.trading" />
        </p>
      )}
      {review ? (
        <div className="creation-review" aria-label={t("启动配置确认")}>
          <dl>
            <dt>{t("实例 ID")}</dt>
            <dd>{review.configuration.id}</dd>
            <dt>{t("已发布版本")}</dt>
            <dd>
              {review.configuration.package?.name} · {review.configuration.package?.version}
            </dd>
            <dt>{t("实际合约")}</dt>
            <dd>{review.configuration.contracts.join(", ")}</dd>
            <dt>{t("K 线周期")}</dt>
            <dd>{review.configuration.period}</dd>
            <dt>{t("参数覆盖")}</dt>
            <dd>
              {Object.entries(review.configuration.params)
                .map(([name, value]) => `${name} = ${parameterText(value)}`)
                .join(", ") || t("使用包内默认参数")}
            </dd>
            {review.configuration.weightBudget && (
              <>
                <dt>{t("总绝对权重预算")}</dt>
                <dd>{weightText(review.configuration.weightBudget.grossUnits!)}</dd>
                <dt>{t("净权重绝对值预算")}</dt>
                <dd>{weightText(review.configuration.weightBudget.netUnits!)}</dd>
              </>
            )}
          </dl>
          {review.policy !== live?.policy_revision && (
            <p role="alert">{t("账户政策已变化，请返回重新核对配置。")}</p>
          )}
          <button disabled={busy} onClick={() => setReview(undefined)}>
            {t("返回修改")}
          </button>
          <button
            className="primary"
            disabled={!usable || busy || review.policy !== live?.policy_revision}
            onClick={() =>
              void start(review.configuration).then(started => {
                if (started) {
                  setReview(undefined);
                  setId("");
                }
              })
            }
          >
            {t("确认启动")}
          </button>
        </div>
      ) : (
        <form
          onSubmit={event => {
            event.preventDefault();
            if (!usable) return;
            setError("");
            try {
              const installed = state.packages.find(
                item => `${item.name}/${item.version}` === packageKey,
              );
              if (!installed) throw new Error(t("请选择主机上已发布的版本。"));
              if (
                !selected.length ||
                selected.length > 20 ||
                selected.some(item => !contracts.includes(item))
              )
                throw new Error(t("请选择账户政策内的 1–20 个实际合约。"));
              setReview({
                policy: live!.policy_revision,
                configuration: create(InstanceSchema, {
                  id: id.trim(),
                  package: { name: installed.name, version: installed.version },
                  contracts: selected,
                  period,
                  params: parameterValues(rows),
                  weightBudget: budget
                    ? { grossUnits: weightUnits(gross), netUnits: weightUnits(net) }
                    : undefined,
                }),
              });
            } catch (reason) {
              setError(asDisplayError(reason));
            }
          }}
        >
          <fieldset disabled={busy || !usable}>
            <div className="futures-fields">
              <label>
                {t("实例 ID")}
                <input
                  required
                  value={id}
                  maxLength={64}
                  pattern="[A-Za-z0-9][A-Za-z0-9._\-]*"
                  onChange={event => setId(event.target.value)}
                />
              </label>
              <label>
                {t("已发布版本")}
                <select
                  required
                  value={packageKey}
                  onChange={event => setPackage(event.target.value)}
                >
                  <option value="">{t("选择策略版本")}</option>
                  {state.packages.map(item => (
                    <option
                      key={`${item.name}/${item.version}`}
                      value={`${item.name}/${item.version}`}
                    >
                      {item.name} · {item.version}
                    </option>
                  ))}
                </select>
              </label>
              <label>
                {t("K 线周期")}
                <select value={period} onChange={event => setPeriod(event.target.value)}>
                  {Array.from({ length: 60 }, (_, i) => `${i + 1}m`)
                    .concat("1d")
                    .map(value => (
                      <option key={value}>{value}</option>
                    ))}
                </select>
              </label>
            </div>
            <p className="subtle">{t("实例 ID 不可复用；重新启动须使用新 ID，并重新预热。")}</p>
            <fieldset className="strategy-contracts">
              <legend>{t("实际合约")}</legend>
              {contracts.map(contract => (
                <label key={contract} className="checkbox">
                  <input
                    type="checkbox"
                    checked={selected.includes(contract)}
                    onChange={event =>
                      setSelected(current =>
                        event.target.checked
                          ? [...current, contract]
                          : current.filter(item => item !== contract),
                      )
                    }
                  />
                  {contract}
                </label>
              ))}
            </fieldset>
            <fieldset>
              <legend>{t("参数覆盖")}</legend>
              <p className="subtle">{t("按策略参数名填写；未覆盖项采用包内默认值。")}</p>
              {rows.map((row, index) => (
                <div className="strategy-parameter" key={row.id}>
                  <label>
                    {t("参数名 {n}", { n: index + 1 })}
                    <input
                      required
                      value={row.name}
                      onChange={event => update(row.id, { name: event.target.value })}
                    />
                  </label>
                  <label>
                    {t("参数类型 {n}", { n: index + 1 })}
                    <select
                      value={row.kind}
                      onChange={event =>
                        update(row.id, {
                          kind: event.target.value as ParameterInput["kind"],
                          value: event.target.value === "flag" ? "false" : "",
                        })
                      }
                    >
                      <option value="integer">{t("整数")}</option>
                      <option value="number">{t("小数")}</option>
                      <option value="flag">{t("布尔")}</option>
                      <option value="text">{t("文本")}</option>
                    </select>
                  </label>
                  <label>
                    {t("参数值 {n}", { n: index + 1 })}
                    {row.kind === "flag" ? (
                      <select
                        value={row.value}
                        onChange={event => update(row.id, { value: event.target.value })}
                      >
                        <option value="false">{t("否")}</option>
                        <option value="true">{t("是")}</option>
                      </select>
                    ) : (
                      <input
                        required={row.kind !== "text"}
                        value={row.value}
                        onChange={event => update(row.id, { value: event.target.value })}
                      />
                    )}
                  </label>
                  <button
                    type="button"
                    onClick={() => setRows(current => current.filter(item => item.id !== row.id))}
                  >
                    {t("删除参数 {n}", { n: index + 1 })}
                  </button>
                </div>
              ))}
              <button
                type="button"
                onClick={() =>
                  setRows(current => [
                    ...current,
                    {
                      id: Math.max(0, ...current.map(item => item.id)) + 1,
                      name: "",
                      kind: "integer",
                      value: "",
                    },
                  ])
                }
              >
                {t("添加参数")}
              </button>
            </fieldset>
            <label className="checkbox">
              <input
                type="checkbox"
                checked={budget}
                onChange={event => setBudget(event.target.checked)}
              />
              {t("策略输出权重，设置权重预算")}
            </label>
            {budget && (
              <div className="futures-fields">
                <label>
                  {t("总绝对权重预算")}
                  <input
                    required
                    inputMode="decimal"
                    value={gross}
                    onChange={event => setGross(event.target.value)}
                  />
                </label>
                <label>
                  {t("净权重绝对值预算")}
                  <input
                    required
                    inputMode="decimal"
                    value={net}
                    onChange={event => setNet(event.target.value)}
                  />
                </label>
              </div>
            )}
            <p className="subtle">
              {t("权重预算按倍数填写，例如 1 表示一倍权益；预算不替代账户风控限额。")}
            </p>
            <button type="submit" disabled={!usable || busy}>
              {t("核对启动配置")}
            </button>
          </fieldset>
        </form>
      )}
    </section>
  );
}
