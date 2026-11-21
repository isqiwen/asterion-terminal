import { listSeparator } from "../../src/i18n";
import { useEffect, useRef, useState } from "react";
import { clone, create } from "@bufbuild/protobuf";
import { Period } from "../../src/generated/asterion/common_pb";
import { InstanceSchema, type Instance } from "../../src/generated/asterion/strategy_pb";
import {
  BrokerState,
  type Account,
  type Strategies,
} from "../../src/generated/asterion/trading_pb";
import { decimal, decimalText } from "../../src/bridge/decimal";
import { lab } from "../../src/bridge/lab";
import { ActionError, useActionError } from "../../src/host/action-error";
import {
  environmentName,
  isProductPolicy,
  key,
  rowProduct,
  t,
  useTradingCatalog,
} from "./live-shared";
import {
  activeStrategy,
  parameterText,
  parameterValues,
  periodText,
  strategyPeriods,
  type ParameterInput,
} from "./strategy-instance";

export function StrategyInstance({
  connection,
  state,
  account,
  available,
  busy,
  published = "",
  start,
}: {
  connection?: string;
  state: Strategies;
  account?: Account;
  available: boolean;
  busy: boolean;
  // The `name/version` just published here, which the form then starts from.
  published?: string;
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
  const [maxMarginRatio, setMaxMarginRatio] = useState("");
  const [minRebalanceWeight, setMinRebalanceWeight] = useState("");
  const [warmupBars, setWarmupBars] = useState("");
  const [localBars, setLocalBars] = useState(false);
  const [review, setReview] = useState<{ configuration: Instance; policy: string }>();
  const [preparing, setPreparing] = useState(false);
  const preparingRef = useRef(false);
  const live = account?.session;
  const policyEntries = live?.policy?.contracts ?? [];
  const { contracts: catalog } = useTradingCatalog(connection ?? "");
  const fixedEntries = policyEntries.filter(item => !isProductPolicy(item));
  const fixedKeys = fixedEntries.map(item => key(item.instrument));
  const seenProducts = new Set<string>();
  const seriesEntries: {
    key: string;
    exchange: string;
    product: string;
    contracts: { instrument: { exchange: string; symbol: string }; deliveryMonth: string }[];
  }[] = [];
  for (const item of policyEntries) {
    const exchange = item.instrument?.exchange ?? "";
    const product = item.product;
    const idKey = `${exchange}.${product}`;
    if (!exchange || !product || seenProducts.has(idKey)) continue;
    const candidates = catalog
      .filter(row => row.instrument?.exchange === exchange && rowProduct(row) === product)
      .map(row => ({
        instrument: { exchange: row.instrument!.exchange, symbol: row.instrument!.symbol },
        deliveryMonth: row.contract.split("/")[2] ?? "",
      }))
      .filter(entry => Boolean(entry.deliveryMonth))
      .sort((a, b) => a.deliveryMonth.localeCompare(b.deliveryMonth));
    if (isProductPolicy(item) || candidates.length >= 2) {
      seenProducts.add(idKey);
      seriesEntries.push({ key: idKey, exchange, product, contracts: candidates });
    }
  }
  const contracts = [...seriesEntries.map(item => item.key), ...fixedKeys];
  const usable =
    available &&
    live?.brokerState === BrokerState.READY &&
    live.positionsReconciled &&
    !live.recoveryRequired;
  const running = activeStrategy(state.instance?.state);
  const ended = state.instance?.instance;
  const installed = state.packages.find(item => `${item.name}/${item.version}` === packageKey);
  const research = installed?.research;
  const choosePackage = (value: string) => {
    setPackage(value);
    const source = state.packages.find(item => `${item.name}/${item.version}` === value)?.research;
    setPeriod(source?.period ?? Period.MINUTES_1);
    setRows(
      Object.entries(source?.params ?? {}).map(([name, parameter], index) => ({
        id: index + 1,
        name,
        kind: parameter.value.case!,
        value: String(parameter.value.value),
      })),
    );
    setBudget(!!source?.weightLimits);
    setGross(source?.weightLimits?.gross?.text ?? "");
    setNet(source?.weightLimits?.net?.text ?? "");
    setMaxMarginRatio(source?.weightLimits?.maxMarginRatio?.text ?? "");
    setMinRebalanceWeight(source?.weightLimits?.minRebalanceWeight?.text ?? "");
    setSelected([]);
  };
  const update = (id: number, change: Partial<ParameterInput>) =>
    setRows(current => current.map(row => (row.id === id ? { ...row, ...change } : row)));
  const taken = useRef("");
  useEffect(() => {
    if (published === taken.current) return;
    if (!state.packages.some(item => `${item.name}/${item.version}` === published)) return;
    taken.current = published;
    choosePackage(published);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [published, state.packages]);

  if (running || !state.packages.length) return null;
  return (
    <section className="strategy-instance" aria-label={t("配置策略实例")}>
      <h2>{t("配置策略实例")}</h2>
      {account && live && (
        <p>
          {t("CTP 账户")} · {account.broker?.brokerId} / {account.broker?.userId} ·{" "}
          {t(environmentName(account.environment))} ·{" "}
          {t(live.authorizedMs !== undefined ? "已允许下单" : "未允许下单")}
        </p>
      )}
      {!usable && <p role="status">{t("账户连接并同步后可启动")}</p>}
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
            {review.configuration.localBars && (
              <>
                <dt>{t("恢复自")}</dt>
                <dd>{t("本地录制的 K 线")}</dd>
              </>
            )}
            {review.configuration.warmup && (
              <>
                <dt>{t("历史预热")}</dt>
                <dd>
                  {review.configuration.warmup.datasetVersion} ·{" "}
                  {review.configuration.warmup.sha256.slice(0, 12)}
                </dd>
              </>
            )}
            <dt>{t("已发布版本")}</dt>
            <dd>
              {review.configuration.packageName} · {review.configuration.packageVersion}
            </dd>
            <dt>{t("实际合约")}</dt>
            <dd>
              {[
                ...review.configuration.instruments.map(key),
                ...review.configuration.series.map(item => `${item.exchange}.${item.product}`),
              ].join(listSeparator())}
            </dd>
            <dt>{t("K 线周期")}</dt>
            <dd>{periodText(review.configuration.period)}</dd>
            <dt>{t("参数覆盖")}</dt>
            <dd>
              {Object.entries(review.configuration.params)
                .map(([name, value]) => `${name} = ${parameterText(value)}`)
                .join(listSeparator()) || t("使用默认参数")}
            </dd>
            {review.configuration.weightLimits && (
              <>
                <dt>{t("总绝对权重预算")}</dt>
                <dd>{decimalText(review.configuration.weightLimits.gross)}</dd>
                <dt>{t("净权重绝对值预算")}</dt>
                <dd>{decimalText(review.configuration.weightLimits.net)}</dd>
                {review.configuration.weightLimits.maxMarginRatio?.text && (
                  <>
                    <dt>{t("保证金率上限")}</dt>
                    <dd>{decimalText(review.configuration.weightLimits.maxMarginRatio)}</dd>
                  </>
                )}
                {review.configuration.weightLimits.minRebalanceWeight?.text && (
                  <>
                    <dt>{t("最小调仓偏离")}</dt>
                    <dd>{decimalText(review.configuration.weightLimits.minRebalanceWeight)}</dd>
                  </>
                )}
              </>
            )}
          </dl>
          {review.policy !== live?.policyRevision && (
            <p role="alert">{t("风控设置已变化，请返回重新核对配置。")}</p>
          )}
          <button type="button" disabled={busy} onClick={() => setReview(undefined)}>
            {t("返回修改")}
          </button>
          <button
            type="button"
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
            if (!usable || preparingRef.current) return;
            preparingRef.current = true;
            setPreparing(true);
            void attempt(async () => {
              if (!installed) throw new Error(t("请选择主机上已发布的版本。"));
              if (
                !selected.length ||
                selected.length > 64 ||
                selected.some(item => !contracts.includes(item))
              )
                throw new Error(t("请选择风控设置内的 1–64 个实际合约。"));
              const chosenFixed = selected
                .filter(item => fixedKeys.includes(item))
                .map(item => fixedEntries[fixedKeys.indexOf(item)]!);
              const chosenSeries = selected
                .filter(item => seriesEntries.some(entry => entry.key === item))
                .map(item => seriesEntries.find(entry => entry.key === item)!);
              if (chosenSeries.some(item => item.contracts.length < 2))
                throw new Error(t("合约目录同步中"));
              if (
                chosenFixed.some(fixed =>
                  chosenSeries.some(
                    series =>
                      series.exchange === fixed.instrument?.exchange &&
                      series.product === fixed.product,
                  ),
                )
              )
                throw new Error(t("请选择风控设置内的 1–64 个实际合约。"));
              const instruments = chosenFixed.map(item => item.instrument!);
              const series = chosenSeries.map(item => ({
                exchange: item.exchange,
                product: item.product,
                contracts: item.contracts,
              }));
              const warmup = warmupBars.trim()
                ? await lab({
                    case: "buildWarmup",
                    value: {
                      instruments: [
                        ...chosenFixed.map(terms => ({
                          instrument: terms.instrument,
                          contract: `${terms.instrument!.exchange}/${terms.product.toLowerCase()}/${terms.deliveryMonth}`,
                        })),
                        ...chosenSeries.map(item => ({
                          instrument: item.contracts[0]!.instrument,
                          contract: `${item.exchange}/${item.product.toLowerCase()}`,
                          candidates: item.contracts,
                        })),
                      ],
                      period,
                      maxBars: Number(warmupBars.trim()),
                    },
                  })
                : undefined;
              setReview({
                policy: live!.policyRevision,
                configuration: create(InstanceSchema, {
                  id: id.trim(),
                  packageName: installed.name,
                  packageVersion: installed.version,
                  instruments,
                  series,
                  period,
                  params: parameterValues(rows),
                  weightLimits: budget
                    ? {
                        gross: decimal(gross),
                        net: decimal(net),
                        maxMarginRatio: maxMarginRatio.trim() ? decimal(maxMarginRatio) : undefined,
                        minRebalanceWeight: minRebalanceWeight.trim()
                          ? decimal(minRebalanceWeight)
                          : undefined,
                      }
                    : undefined,
                  warmup,
                  localBars,
                }),
              });
            }).finally(() => {
              preparingRef.current = false;
              setPreparing(false);
            });
          }}
        >
          <fieldset disabled={busy || preparing || !usable}>
            <div className="futures-fields">
              <label>
                {t("实例 ID")}
                <input
                  required
                  value={id}
                  maxLength={64}
                  pattern="[A-Za-z0-9][A-Za-z0-9._\-]*"
                  spellCheck={false}
                  autoCorrect="off"
                  autoCapitalize="none"
                  onChange={event => setId(event.target.value)}
                />
              </label>
              <label>
                {t("已发布版本")}
                <select
                  required
                  value={packageKey}
                  onChange={event => choosePackage(event.target.value)}
                >
                  <option value="">{t("选择策略版本")}</option>
                  {state.packages.map(item => (
                    <option
                      key={`${item.name}/${item.version}`}
                      value={`${item.name}/${item.version}`}
                    >
                      {item.name.split(".").at(-1)} · {item.version}
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
                  {strategyPeriods.map(value => (
                    <option key={value} value={value}>
                      {periodText(value)}
                    </option>
                  ))}
                </select>
              </label>
              <label>
                {t("历史预热 K 线数")}
                <input
                  type="number"
                  min={1}
                  max={500}
                  value={warmupBars}
                  onChange={event => setWarmupBars(event.target.value)}
                />
              </label>
              <label className="checkbox">
                <input
                  type="checkbox"
                  checked={localBars}
                  onChange={event => setLocalBars(event.target.checked)}
                />
                {t("从本地录制的 K 线恢复")}
              </label>
            </div>
            {ended && (
              <button
                type="button"
                disabled={
                  !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(id.trim()) ||
                  Boolean(packageKey && !packageKey.startsWith(`${ended.packageName}/`))
                }
                onClick={() =>
                  void attempt(async () => {
                    const configuration = clone(InstanceSchema, ended);
                    configuration.id = id.trim();
                    configuration.resumeFrom = ended.id;
                    // The record already holds the ended instance's warmup bars.
                    configuration.localBars = false;
                    configuration.warmup = undefined;
                    const installed = state.packages.find(
                      item =>
                        `${item.name}/${item.version}` === packageKey &&
                        item.name === ended.packageName,
                    );
                    if (installed) configuration.packageVersion = installed.version;
                    if (rows.length > 0) configuration.params = parameterValues(rows);
                    if (budget)
                      configuration.weightLimits = create(InstanceSchema, {
                        weightLimits: {
                          gross: decimal(gross),
                          net: decimal(net),
                          maxMarginRatio: maxMarginRatio.trim()
                            ? decimal(maxMarginRatio)
                            : undefined,
                          minRebalanceWeight: minRebalanceWeight.trim()
                            ? decimal(minRebalanceWeight)
                            : undefined,
                        },
                      }).weightLimits;
                    setReview({ policy: live!.policyRevision, configuration });
                  })
                }
              >
                {t("从实例 {id} 的记录恢复", { id: ended.id })}
              </button>
            )}
            <fieldset className="strategy-contracts">
              <legend>{t("实际合约")}</legend>
              {research && (
                <p>
                  {t("来源回测")} · {research.runId} · {research.symbols.join(listSeparator())}
                </p>
              )}
              {seriesEntries.map(entry => (
                <label key={entry.key} className="checkbox">
                  <input
                    type="checkbox"
                    checked={selected.includes(entry.key)}
                    onChange={event =>
                      setSelected(current =>
                        event.target.checked
                          ? [...current, entry.key]
                          : current.filter(item => item !== entry.key),
                      )
                    }
                  />
                  {`${entry.key} · ${t("主力连续")}`}
                </label>
              ))}
              {fixedKeys.map(contract => (
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
              {rows.map((row, index) => (
                <div className="strategy-parameter" key={row.id}>
                  <label>
                    {t("参数名 {n}", { n: index + 1 })}
                    <input
                      required
                      spellCheck={false}
                      autoCorrect="off"
                      autoCapitalize="none"
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
              {t("设置策略权重预算")}
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
                <label>
                  {t("保证金率上限")}
                  <input
                    inputMode="decimal"
                    value={maxMarginRatio}
                    onChange={event => setMaxMarginRatio(event.target.value)}
                  />
                </label>
                <label>
                  {t("最小调仓偏离")}
                  <input
                    inputMode="decimal"
                    value={minRebalanceWeight}
                    onChange={event => setMinRebalanceWeight(event.target.value)}
                  />
                </label>
              </div>
            )}
            <button type="submit" className="primary" disabled={!usable || busy || preparing}>
              {t(preparing ? "正在准备预热数据…" : "核对启动配置")}
            </button>
            <ActionError error={error} namespace="asterion.terminal.trading" />
          </fieldset>
        </form>
      )}
    </section>
  );
}
