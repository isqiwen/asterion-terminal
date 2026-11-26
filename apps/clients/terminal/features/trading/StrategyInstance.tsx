import { listSeparator } from "../../src/i18n";
import { useEffect, useRef, useState } from "react";
import { clone, create } from "@bufbuild/protobuf";
import { Period } from "../../src/generated/asterion/common_pb";
import {
  InstanceSchema,
  type Instance,
  type ParameterValue,
} from "../../src/generated/asterion/strategy_pb";
import type { StrategyEntry } from "../../src/generated/asterion/lab_pb";
import {
  BrokerState,
  type Account,
  type Strategies,
} from "../../src/generated/asterion/trading_pb";
import { decimal, decimalText } from "../../src/bridge/decimal";
import { lab } from "../../src/bridge/lab";
import { ActionError, useActionError } from "../../src/host/action-error";
import { isProductPolicy, key, rowProduct, t, useTradingCatalog } from "./live-shared";
import {
  activeStrategy,
  parameterText,
  parameterValues,
  periodText,
  strategyPeriods,
  type ParameterInput,
} from "./strategy-instance";

const parameterKind = (value?: ParameterValue) =>
  (value?.value.case ?? "text") as ParameterInput["kind"];
const shownValue = (value?: ParameterValue) =>
  value?.value.case === undefined ? "" : String(value.value.value);

function suggestInstanceId(packageName: string, avoid?: string) {
  const short = (packageName.split(".").at(-1) || packageName)
    .replace(/[^A-Za-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .toLowerCase();
  const base = short || "run";
  const stamp = new Date()
    .toLocaleString("sv-SE", { timeZone: "Asia/Shanghai", hour12: false })
    .replace(/[-: ]/g, "")
    .slice(4, 12);
  const candidate = `${base}-${stamp}`.slice(0, 60);
  return candidate === avoid ? `${candidate}-2` : candidate;
}

export function StrategyInstance({
  connection,
  repository,
  state,
  account,
  available,
  busy,
  published = "",
  deployFirst = false,
  start,
}: {
  connection?: string;
  // Where Lab reads the parameters each strategy declares.
  repository?: string;
  state: Strategies;
  account?: Account;
  available: boolean;
  busy: boolean;
  // The `name/version` just published here, which the form then starts from.
  published?: string;
  // Deploying comes first, so starting is not the highlighted step.
  deployFirst?: boolean;
  start: (configuration: Instance) => Promise<boolean>;
}) {
  const { error, attempt } = useActionError();
  const [id, setId] = useState("");
  const autoIdRef = useRef("");
  const [packageKey, setPackage] = useState("");
  const [selected, setSelected] = useState<string[]>([]);
  const [contractFilter, setContractFilter] = useState("");
  const [period, setPeriod] = useState(Period.MINUTES_1);
  const [declared, setDeclared] = useState<readonly StrategyEntry[]>([]);
  // What was typed for each parameter; a blank one keeps its default.
  const [values, setValues] = useState<Record<string, string>>({});
  const [budget, setBudget] = useState(false);
  const [gross, setGross] = useState("");
  const [net, setNet] = useState("");
  const [maxMarginRatio, setMaxMarginRatio] = useState("");
  const [minRebalanceWeight, setMinRebalanceWeight] = useState("");
  const [customExec, setCustomExec] = useState(false);
  const [aggressiveTicks, setAggressiveTicks] = useState("0");
  const [maxReprices, setMaxReprices] = useState("0");
  const [repriceIntervalMs, setRepriceIntervalMs] = useState("3000");
  const [twapSlices, setTwapSlices] = useState("1");
  const [twapIntervalMs, setTwapIntervalMs] = useState("5000");
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
    const chosen = state.packages.find(item => `${item.name}/${item.version}` === value);
    const source = chosen?.research;
    if (chosen && (!id.trim() || id === autoIdRef.current)) {
      const nextId = suggestInstanceId(chosen.name, ended?.id);
      autoIdRef.current = nextId;
      setId(nextId);
    }
    setPeriod(source?.period ?? Period.MINUTES_1);
    setValues(
      Object.fromEntries(
        Object.entries(source?.params ?? {}).map(([name, paramValue]) => [
          name,
          shownValue(paramValue),
        ]),
      ),
    );
    setBudget(!!source?.weightLimits);
    setGross(source?.weightLimits?.gross?.text ?? "");
    setNet(source?.weightLimits?.net?.text ?? "");
    setMaxMarginRatio(source?.weightLimits?.maxMarginRatio?.text ?? "");
    setMinRebalanceWeight(source?.weightLimits?.minRebalanceWeight?.text ?? "");
    setSelected([]);
  };
  useEffect(() => {
    if (!repository) return;
    let current = true;
    // Without Lab the version's own backtest values are still listed.
    void lab({ case: "listStrategies", value: { path: repository } })
      .then(listed => current && setDeclared(listed.strategies))
      .catch(() => {});
    return () => {
      current = false;
    };
  }, [repository, published]);
  // A version from a backtest brings its own full set of values.
  const resumed =
    ended && !research && (!installed || installed.name === ended.packageName) ? ended : undefined;
  const parameters = new Map<string, { kind: ParameterInput["kind"]; placeholder: string }>();
  const strategyName = installed?.name ?? ended?.packageName;
  for (const item of declared.find(entry => entry.id === strategyName)?.parameters ?? [])
    parameters.set(item.name, {
      kind: parameterKind(item.default),
      placeholder: shownValue(item.default),
    });
  for (const [name, value] of Object.entries(research?.params ?? {}))
    if (!parameters.has(name))
      parameters.set(name, { kind: parameterKind(value), placeholder: "" });
  for (const [name, value] of Object.entries(resumed?.params ?? {}))
    parameters.set(name, { kind: parameterKind(value), placeholder: shownValue(value) });
  const typedParameters = () =>
    parameterValues(
      [...parameters]
        .filter(([name]) => values[name]?.trim())
        .map(([name, { kind }]) => ({ name, kind, value: values[name]!.trim() })),
    );
  const taken = useRef("");
  useEffect(() => {
    if (published === taken.current) return;
    if (!state.packages.some(item => `${item.name}/${item.version}` === published)) return;
    taken.current = published;
    choosePackage(published);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [published, state.packages]);

  if (running || !state.packages.length) return null;
  const query = contractFilter.trim().toLowerCase();
  const visibleSeries = query
    ? seriesEntries.filter(entry => entry.key.toLowerCase().includes(query))
    : seriesEntries;
  const visibleFixed = query
    ? fixedKeys.filter(item => item.toLowerCase().includes(query))
    : fixedKeys;

  const researchMatches = research?.symbols.length
    ? [
        ...new Set(
          research.symbols.flatMap(raw => {
            const [ex, prod] = raw.split("/");
            if (!ex || !prod) return [];
            const seriesMatch = seriesEntries.find(
              entry =>
                entry.exchange.toUpperCase() === ex.toUpperCase() &&
                entry.product.toLowerCase() === prod.toLowerCase(),
            );
            if (seriesMatch) return [seriesMatch.key];
            return fixedEntries
              .filter(
                entry =>
                  entry.instrument?.exchange.toUpperCase() === ex.toUpperCase() &&
                  entry.product.toLowerCase() === prod.toLowerCase(),
              )
              .map(entry => key(entry.instrument));
          }),
        ),
      ]
    : [];

  const validId = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(id.trim());
  const resumeBlockedByPackage = Boolean(
    packageKey && ended && !packageKey.startsWith(`${ended.packageName}/`),
  );

  const buildExecution = () => {
    if (!customExec) return undefined;
    const agg = Number(aggressiveTicks.trim() || "0");
    const reprices = Number(maxReprices.trim() || "0");
    const repriceMs = Number(repriceIntervalMs.trim() || "0");
    const slices = Number(twapSlices.trim() || "1");
    const sliceMs = Number(twapIntervalMs.trim() || "0");
    if (!Number.isInteger(agg) || agg < 0 || agg > 5)
      throw new Error(t("超价跳数须为 0–5 的整数。"));
    if (!Number.isInteger(reprices) || reprices < 0 || reprices > 5)
      throw new Error(t("最大追价次数须为 0–5 的整数。"));
    if (reprices > 0 && (!Number.isInteger(repriceMs) || repriceMs < 1000 || repriceMs > 30000))
      throw new Error(t("追价间隔须为 1000–30000 毫秒的整数。"));
    if (!Number.isInteger(slices) || slices < 1 || slices > 10)
      throw new Error(t("TWAP 切片数须为 1–10 的整数。"));
    if (slices > 1 && (!Number.isInteger(sliceMs) || sliceMs < 1000 || sliceMs > 60000))
      throw new Error(t("切片间隔须为 1000–60000 毫秒的整数。"));
    if (agg === 0 && reprices === 0 && slices <= 1) return undefined;
    return {
      aggressiveTicks: agg,
      maxReprices: reprices,
      repriceIntervalMs: reprices > 0 ? repriceMs : 0,
      twapSlices: slices,
      twapIntervalMs: slices > 1 ? sliceMs : 0,
    };
  };

  const formatExecution = (exec?: Instance["execution"]) => {
    if (!exec) return "";
    const parts: string[] = [];
    if (exec.aggressiveTicks > 0) parts.push(t("超价 {n} 跳", { n: String(exec.aggressiveTicks) }));
    if (exec.maxReprices > 0)
      parts.push(
        t("追价 {n} 次 / {ms}ms", {
          n: String(exec.maxReprices),
          ms: String(exec.repriceIntervalMs || 3000),
        }),
      );
    if (exec.twapSlices > 1)
      parts.push(
        t("TWAP {n} 片 / {ms}ms", {
          n: String(exec.twapSlices),
          ms: String(exec.twapIntervalMs || 5000),
        }),
      );
    return parts.join(" · ");
  };

  return (
    <section className="strategy-instance" aria-label={t("配置策略实例")}>
      <h2>{t("配置策略实例")}</h2>
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
            <dt>{t("已部署版本")}</dt>
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
            <dt>{t("参数")}</dt>
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
            {review.configuration.execution && formatExecution(review.configuration.execution) && (
              <>
                <dt>{t("实盘执行选项")}</dt>
                <dd>{formatExecution(review.configuration.execution)}</dd>
              </>
            )}
          </dl>
          {review.policy !== live?.policyRevision && (
            <p role="alert">{t("风控设置已变化，请返回重新核对配置。")}</p>
          )}
          <div className="workflow-actions">
            <button type="button" disabled={busy} onClick={() => setReview(undefined)}>
              {t("返回修改")}
            </button>
            <button
              type="button"
              className={deployFirst ? undefined : "primary"}
              disabled={!usable || busy || review.policy !== live?.policyRevision}
              onClick={() =>
                void start(review.configuration).then(started => {
                  if (started) {
                    setReview(undefined);
                    autoIdRef.current = "";
                    setId("");
                  }
                })
              }
            >
              {t("确认启动")}
            </button>
          </div>
        </div>
      ) : (
        <form
          onSubmit={event => {
            event.preventDefault();
            if (!usable || preparingRef.current) return;
            preparingRef.current = true;
            setPreparing(true);
            void attempt(async () => {
              if (!installed) throw new Error(t("请选择已部署的版本。"));
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
                  params: typedParameters(),
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
                  execution: buildExecution(),
                }),
              });
            }).finally(() => {
              preparingRef.current = false;
              setPreparing(false);
            });
          }}
        >
          <fieldset disabled={busy || preparing || !usable}>
            {ended && (
              <div className="strategy-resume-bar">
                <button
                  type="button"
                  disabled={!validId || resumeBlockedByPackage}
                  onClick={() =>
                    void attempt(async () => {
                      const configuration = clone(InstanceSchema, ended);
                      configuration.id = id.trim();
                      configuration.resumeFrom = ended.id;
                      // The record already holds the ended instance's warmup bars.
                      configuration.localBars = false;
                      configuration.warmup = undefined;
                      const matched = state.packages.find(
                        item =>
                          `${item.name}/${item.version}` === packageKey &&
                          item.name === ended.packageName,
                      );
                      if (matched) configuration.packageVersion = matched.version;
                      configuration.params = research
                        ? typedParameters()
                        : { ...ended.params, ...typedParameters() };
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
                      const exec = buildExecution();
                      if (exec)
                        configuration.execution = create(InstanceSchema, {
                          execution: exec,
                        }).execution;
                      setReview({ policy: live!.policyRevision, configuration });
                    })
                  }
                >
                  {t("从实例 {id} 的记录恢复", { id: ended.id })}
                </button>
                {!validId && (
                  <span className="subtle">{t("填写新实例 ID 后可沿用上次合约与预热记录")}</span>
                )}
              </div>
            )}
            <div className="futures-fields">
              <label>
                {t("实例 ID")}
                <input
                  required
                  value={id}
                  maxLength={64}
                  pattern="[A-Za-z0-9][A-Za-z0-9._\-]*"
                  placeholder={suggestInstanceId(strategyName ?? "run", ended?.id)}
                  spellCheck={false}
                  autoCorrect="off"
                  autoCapitalize="none"
                  onChange={event => setId(event.target.value)}
                />
              </label>
              <label>
                {t("已部署版本")}
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
            <fieldset className="strategy-contracts">
              <legend>{t("实际合约")}</legend>
              {research && (
                <div className="strategy-research-match">
                  <p>
                    {t("来源回测")} · {research.runId} · {research.symbols.join(listSeparator())}
                  </p>
                  {researchMatches.length > 0 && (
                    <button type="button" onClick={() => setSelected(researchMatches)}>
                      {t("按来源品种勾选")}
                    </button>
                  )}
                </div>
              )}
              {contracts.length > 8 && (
                <div className="strategy-contracts-bar">
                  <input
                    type="search"
                    aria-label={t("筛选合约")}
                    placeholder={t("筛选合约")}
                    value={contractFilter}
                    onChange={event => setContractFilter(event.target.value)}
                  />
                  <button
                    type="button"
                    onClick={() =>
                      setSelected([
                        ...new Set([
                          ...selected,
                          ...visibleSeries.map(item => item.key),
                          ...visibleFixed,
                        ]),
                      ])
                    }
                  >
                    {t("全选")}
                  </button>
                  <button type="button" disabled={!selected.length} onClick={() => setSelected([])}>
                    {t("清空")}
                  </button>
                </div>
              )}
              {visibleSeries.map(entry => (
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
              {visibleFixed.map(contract => (
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
            {parameters.size > 0 && (
              <fieldset>
                <legend>{t("参数")}</legend>
                <div className="futures-fields">
                  {[...parameters].map(([name, { kind, placeholder }]) => (
                    <label key={name}>
                      {name}
                      {kind === "flag" ? (
                        <select
                          value={values[name] ?? ""}
                          onChange={event =>
                            setValues(current => ({ ...current, [name]: event.target.value }))
                          }
                        >
                          <option value="">
                            {t("默认")}
                            {placeholder && ` (${t(placeholder === "true" ? "是" : "否")})`}
                          </option>
                          <option value="true">{t("是")}</option>
                          <option value="false">{t("否")}</option>
                        </select>
                      ) : (
                        <input
                          inputMode={kind === "text" ? undefined : "decimal"}
                          spellCheck={false}
                          autoCorrect="off"
                          autoCapitalize="none"
                          placeholder={placeholder}
                          value={values[name] ?? ""}
                          onChange={event =>
                            setValues(current => ({ ...current, [name]: event.target.value }))
                          }
                        />
                      )}
                    </label>
                  ))}
                </div>
              </fieldset>
            )}
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
            <label className="checkbox">
              <input
                type="checkbox"
                checked={customExec}
                onChange={event => setCustomExec(event.target.checked)}
              />
              {t("设置实盘执行选项")}
            </label>
            {customExec && (
              <div className="futures-fields">
                <label>
                  {t("超价跳数")}
                  <input
                    type="number"
                    min={0}
                    max={5}
                    value={aggressiveTicks}
                    onChange={event => setAggressiveTicks(event.target.value)}
                  />
                </label>
                <label>
                  {t("最大追价次数")}
                  <input
                    type="number"
                    min={0}
                    max={5}
                    value={maxReprices}
                    onChange={event => setMaxReprices(event.target.value)}
                  />
                </label>
                <label>
                  {t("追价间隔（毫秒）")}
                  <input
                    type="number"
                    min={1000}
                    max={30000}
                    value={repriceIntervalMs}
                    onChange={event => setRepriceIntervalMs(event.target.value)}
                  />
                </label>
                <label>
                  {t("TWAP 切片数")}
                  <input
                    type="number"
                    min={1}
                    max={10}
                    value={twapSlices}
                    onChange={event => setTwapSlices(event.target.value)}
                  />
                </label>
                <label>
                  {t("切片间隔（毫秒）")}
                  <input
                    type="number"
                    min={1000}
                    max={60000}
                    value={twapIntervalMs}
                    onChange={event => setTwapIntervalMs(event.target.value)}
                  />
                </label>
              </div>
            )}
            <button
              type="submit"
              className={deployFirst ? undefined : "primary"}
              disabled={!usable || busy || preparing}
            >
              {t(preparing ? "正在准备预热数据…" : "核对启动配置")}
            </button>
            <ActionError error={error} namespace="asterion.terminal.trading" />
          </fieldset>
        </form>
      )}
    </section>
  );
}
