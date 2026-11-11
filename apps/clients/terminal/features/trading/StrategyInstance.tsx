import { useState } from "react";
import { clone, create } from "@bufbuild/protobuf";
import { Period } from "../../src/generated/asterion/common_pb";
import { InstanceSchema, type Instance } from "../../src/generated/asterion/strategy_pb";
import {
  BrokerState,
  type Account,
  type Strategies,
} from "../../src/generated/asterion/trading_pb";
import { decimal, decimalText } from "../../src/bridge/decimal";
import { ActionError, useActionError } from "../../src/host/action-error";
import { environmentName, key, t } from "./live-shared";
import {
  activeStrategy,
  parameterText,
  parameterValues,
  periodText,
  strategyPeriods,
  type ParameterInput,
} from "./strategy-instance";

export function StrategyInstance({
  state,
  account,
  available,
  busy,
  start,
}: {
  state: Strategies;
  account?: Account;
  available: boolean;
  busy: boolean;
  start: (configuration: Instance) => Promise<boolean>;
}) {
  const { error, attempt } = useActionError();
  const [id, setId] = useState("");
  const [packageKey, setPackage] = useState("");
  const [selected, setSelected] = useState<string[]>([]);
  const [period, setPeriod] = useState(Period.MINUTES_1);
  const [rows, setRows] = useState<ParameterInput[]>([]);
  const [budget, setBudget] = useState(false);
  const [gross, setGross] = useState("");
  const [net, setNet] = useState("");
  const [review, setReview] = useState<{ configuration: Instance; policy: string }>();
  const live = account?.session;
  const allowed = live?.policy?.contracts.map(item => item.instrument!) ?? [];
  const contracts = allowed.map(key);
  const usable =
    available &&
    live?.brokerState === BrokerState.READY &&
    live.positionsReconciled &&
    !live.recoveryRequired;
  const running = activeStrategy(state.instance?.state);
  const ended = state.instance?.instance;
  const update = (id: number, change: Partial<ParameterInput>) =>
    setRows(current => current.map(row => (row.id === id ? { ...row, ...change } : row)));

  if (running || !state.packages.length) return null;
  return (
    <section className="strategy-instance" aria-label={t("配置策略实例")}>
      <h4>{t("配置策略实例")}</h4>
      {account && live && (
        <p>
          {t("CTP 账户")} · {account.broker?.brokerId} / {account.broker?.userId} ·{" "}
          {t("主机配置的柜台环境：{environment}", {
            environment: t(environmentName(account.environment)),
          })}{" "}
          · {t(live.authorizedMs !== undefined ? "已授权" : "尚未允许发送委托")}
        </p>
      )}
      {!usable && <p role="status">{t("请先连接并同步账户，再启动实例。")}</p>}
      <p className="subtle">
        {t(
          "启动从空窗口现场预热，或从已结束实例的记录恢复。启动不授权；账户未授权时，就绪目标只记录、不执行。",
        )}
      </p>
      {review ? (
        <div className="creation-review" aria-label={t("启动配置确认")}>
          <dl>
            <dt>{t("实例 ID")}</dt>
            <dd>{review.configuration.id}</dd>
            {review.configuration.resumeFrom && (
              <>
                <dt>{t("恢复自")}</dt>
                <dd>{t("实例 {id} 的记录", { id: review.configuration.resumeFrom })}</dd>
              </>
            )}
            <dt>{t("已发布版本")}</dt>
            <dd>
              {review.configuration.packageName} · {review.configuration.packageVersion}
            </dd>
            <dt>{t("实际合约")}</dt>
            <dd>{review.configuration.instruments.map(key).join(", ")}</dd>
            <dt>{t("K 线周期")}</dt>
            <dd>{periodText(review.configuration.period)}</dd>
            <dt>{t("参数覆盖")}</dt>
            <dd>
              {Object.entries(review.configuration.params)
                .map(([name, value]) => `${name} = ${parameterText(value)}`)
                .join(", ") || t("使用包内默认参数")}
            </dd>
            {review.configuration.weightLimits && (
              <>
                <dt>{t("总绝对权重预算")}</dt>
                <dd>{decimalText(review.configuration.weightLimits.gross)}</dd>
                <dt>{t("净权重绝对值预算")}</dt>
                <dd>{decimalText(review.configuration.weightLimits.net)}</dd>
              </>
            )}
          </dl>
          {review.policy !== live?.policyRevision && (
            <p role="alert">{t("账户政策已变化，请返回重新核对配置。")}</p>
          )}
          <button disabled={busy} onClick={() => setReview(undefined)}>
            {t("返回修改")}
          </button>
          <button
            className="primary"
            disabled={!usable || busy || review.policy !== live?.policyRevision}
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
            void attempt(async () => {
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
                policy: live!.policyRevision,
                configuration: create(InstanceSchema, {
                  id: id.trim(),
                  packageName: installed.name,
                  packageVersion: installed.version,
                  instruments: selected.map(item => allowed[contracts.indexOf(item)]!),
                  period,
                  params: parameterValues(rows),
                  weightLimits: budget ? { gross: decimal(gross), net: decimal(net) } : undefined,
                }),
              });
            });
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
                <select
                  value={period}
                  onChange={event => setPeriod(Number(event.target.value) as Period)}
                >
                  {strategyPeriods.map(([value, label]) => (
                    <option key={value} value={value}>
                      {label}
                    </option>
                  ))}
                </select>
              </label>
            </div>
            <p className="subtle">{t("实例 ID 不可复用；重新启动须使用新 ID。")}</p>
            {ended && (
              <button
                type="button"
                disabled={!id.trim()}
                onClick={() => {
                  const configuration = clone(InstanceSchema, ended);
                  configuration.id = id.trim();
                  configuration.resumeFrom = ended.id;
                  setReview({ policy: live!.policyRevision, configuration });
                }}
              >
                {t("从实例 {id} 的记录恢复", { id: ended.id })}
              </button>
            )}
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
            <ActionError error={error} namespace="asterion.terminal.trading" />
          </fieldset>
        </form>
      )}
    </section>
  );
}
