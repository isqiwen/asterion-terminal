import type { CommandParams } from "../../src/bridge/commands";
import { useCallback, useEffect, useRef, useState } from "react";
import {
  useWorkspaceDraft,
  useWorkspaceRequestId,
  dailyTableOf,
  DatasetPicker,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type {
  ResearchVariableDefinition,
  FactorKind,
  TableCoverage,
  TableProduct,
} from "../../src/bridge/client";
import { factorNames, factorWords, t, type Run, selectedSeries } from "./shared";

type Kind = keyof typeof factorWords;
type VariableSelection = {
  variableDefinition?: ResearchVariableDefinition | null;
  variableReady?: boolean;
};
const defaults = {
  retain_observations: false,
  lookback: "5",
  horizon: "1",
  evaluation: "full_sample",
  split: "",
  training: "",
  validation: "",
};

/**
 * What every factor task is given besides its series: windows, horizon and how
 * it is evaluated. `series` is what would be submitted, or null while nothing
 * is selected; `identity` changes when that selection does.
 */
function FactorParameters({
  kind,
  factor = "momentum",
  series,
  identity,
  observations,
  selectionCount = 0,
  variableDefinition,
  variableReady = true,
  snapshot,
  busy,
  run,
}: Pick<TerminalContext, "snapshot" | "busy"> &
  VariableSelection & {
    kind: Kind;
    factor?: FactorKind;
    series: CommandParams["factor.submit"]["series"] | null;
    identity: string;
    observations?: number;
    selectionCount?: number;
    run: Run;
  }) {
  const words = factorWords[kind];
  const taskService = snapshot?.research_service;
  const destination = JSON.stringify([snapshot?.data?.connection_id, taskService?.connection_id]);
  const [parameters, setParameters] = useWorkspaceDraft(`factor:${kind}`, defaults);
  const [liquidity, setLiquidity] = useWorkspaceDraft(`factor:${kind}:liquidity`, {
    enabled: false,
    history: "5",
    volume: "0",
    interest: "0",
  });
  const filtered = kind === "tables" && liquidity.enabled;
  const expressionSupported =
    !variableDefinition ||
    (selectionCount === 1 &&
      !filtered &&
      (parameters.evaluation === "full_sample" || parameters.evaluation === "holdout") &&
      parameters.retain_observations);
  const observationSupport = variableDefinition
    ? selectionCount === 1 &&
      !filtered &&
      (parameters.evaluation === "full_sample" || parameters.evaluation === "holdout")
    : selectionCount === 1 &&
      (parameters.evaluation !== "full_sample" ||
        parameters.lookback.split(",").filter(value => value.trim()).length === 1) &&
      !filtered &&
      (factor === "momentum" || factor === "volatility");
  const [submitted, setSubmitted] = useWorkspaceDraft(
    `submitted:${destination}:factor:${kind}`,
    "",
  );
  const [pendingId, setPendingId] = useWorkspaceRequestId(
    `factor-submission:${kind}`,
    JSON.stringify([
      destination,
      identity,
      factor,
      parameters,
      filtered ? liquidity : null,
      variableDefinition?.object,
    ]),
  );
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    const id = pendingId ?? `factor-${crypto.randomUUID()}`;
    setPendingId(id);
    if (!series || !factor || !variableReady || !expressionSupported) return;
    const payload: CommandParams["factor.submit"] = {
      id,
      series,
      factor: variableDefinition ? "expression" : factor,
      ...(variableDefinition ? { variable_definition: variableDefinition.object } : {}),
      lookbacks: variableDefinition
        ? [variableDefinition.window]
        : parameters.lookback.split(",").map(value => Number(value.trim())),
      horizon: Number(parameters.horizon),
      ...(parameters.retain_observations ? { retain_observations: true } : {}),
      ...(filtered
        ? {
            liquidity: {
              minimum_history: Number(liquidity.history),
              minimum_volume: liquidity.volume,
              minimum_open_interest: liquidity.interest,
            },
          }
        : {}),
      evaluation:
        parameters.evaluation === "full_sample"
          ? { mode: "full_sample" }
          : parameters.evaluation === "walk_forward"
            ? {
                mode: "walk_forward",
                training_events: Number(parameters.training),
                validation_events: Number(parameters.validation),
              }
            : { mode: "holdout", split_index: Number(parameters.split) },
    };
    if (await run("factor.submit", payload)) {
      setSubmitted(id);
      setPendingId(null);
    }
  }
  return (
    <>
      {variableDefinition && (
        <p role="status">
          {t(
            "固定表达式支持单序列、固定窗口、全样本或时间留出，并须保存逐行证据；请明确选择这些条件。",
          )}
        </p>
      )}
      <form onSubmit={event => void submit(event)}>
        <fieldset disabled={busy}>
          <div className="research-fields">
            {(["lookback", "horizon"] as const).map(key => (
              <label key={key}>
                {t(words[key])}
                <input
                  aria-label={t(words[key])}
                  type={key === "lookback" ? "text" : "number"}
                  inputMode={key === "lookback" ? "text" : "numeric"}
                  pattern={
                    key === "lookback" ? "[ ]*[1-9][0-9]*[ ]*(,[ ]*[1-9][0-9]*[ ]*)*" : undefined
                  }
                  min="1"
                  max="10000"
                  step="1"
                  required
                  readOnly={key === "lookback" && Boolean(variableDefinition)}
                  value={
                    key === "lookback" && variableDefinition
                      ? String(variableDefinition.window)
                      : parameters[key]
                  }
                  onChange={e => setParameters({ ...parameters, [key]: e.target.value })}
                />
              </label>
            ))}
          </div>
          {kind === "tables" && (
            <fieldset>
              <legend>{t("历史流动性筛选")}</legend>
              <label>
                <input
                  type="checkbox"
                  checked={liquidity.enabled}
                  onChange={e => setLiquidity({ ...liquidity, enabled: e.target.checked })}
                />
                {t("启用历史流动性筛选")}
              </label>
              {filtered && (
                <>
                  <div className="research-fields">
                    <label>
                      {t("此前连续观测数")}
                      <input
                        type="number"
                        min="1"
                        max="10000"
                        step="1"
                        required
                        value={liquidity.history}
                        onChange={e => setLiquidity({ ...liquidity, history: e.target.value })}
                      />
                    </label>
                    <label>
                      {t("上一观测最低成交量（手）")}
                      <input
                        type="text"
                        inputMode="numeric"
                        pattern="[0-9]+"
                        required
                        value={liquidity.volume}
                        onChange={e => setLiquidity({ ...liquidity, volume: e.target.value })}
                      />
                    </label>
                    <label>
                      {t("上一观测最低持仓量（手）")}
                      <input
                        type="text"
                        inputMode="numeric"
                        pattern="[0-9]+"
                        required
                        value={liquidity.interest}
                        onChange={e => setLiquidity({ ...liquidity, interest: e.target.value })}
                      />
                    </label>
                  </div>
                  <p className="subtle">
                    {t(
                      "筛选仅支持至少 3 个品种或合约的日线截面。使用此前连续收盘观测及上一观测的原始合约数量；未知不当作零，阈值包含等号。输入起点之前的历史不假定存在，固定快照不证明历史发布时点。",
                    )}
                  </p>
                </>
              )}
            </fieldset>
          )}
          <label>
            {t("评价方式")}
            <select
              aria-label={t("评价方式")}
              value={parameters.evaluation}
              onChange={e => setParameters({ ...parameters, evaluation: e.target.value })}
            >
              <option value="full_sample">{t("全样本评价")}</option>
              <option value="holdout">{t("时间留出评价")}</option>
              <option value="walk_forward">{t("滚动验证")}</option>
            </select>
          </label>
          {parameters.evaluation === "holdout" && (
            <label>
              {t(words.split)}
              <input
                aria-label={t(words.split)}
                type="number"
                min="1"
                max={observations ? observations - 1 : 19999}
                step="1"
                required
                value={parameters.split}
                onChange={e => setParameters({ ...parameters, split: e.target.value })}
              />
            </label>
          )}
          {parameters.evaluation === "walk_forward" && (
            <>
              <div className="research-fields">
                {(["training", "validation"] as const).map(key => (
                  <label key={key}>
                    {t(words[key])}
                    <input
                      aria-label={t(words[key])}
                      type="number"
                      min="1"
                      max="10000"
                      step="1"
                      required
                      value={parameters[key]}
                      onChange={e => setParameters({ ...parameters, [key]: e.target.value })}
                    />
                  </label>
                ))}
              </div>
              <p className="subtle">
                {t("固定训练窗口逐轮前移，验证段须完整覆盖剩余数据，共 2–16 轮。")}
              </p>
            </>
          )}
          {!variableDefinition && (
            <p className="subtle">
              {t("多个窗口按递增顺序用逗号分隔，最多 32 个，需使用留出或滚动验证。")}
            </p>
          )}
          <p className="subtle">
            {t("每段至少 30 个有标签观察；无效特征另行排除，跨分界标签不参与评价。")}
          </p>
          <label>
            <input
              type="checkbox"
              checked={parameters.retain_observations}
              disabled={!observationSupport && !parameters.retain_observations}
              onChange={e =>
                setParameters({ ...parameters, retain_observations: e.target.checked })
              }
            />
            {t("保存逐行特征与标签证据")}
          </label>
          <p className="subtle">
            {t(
              "逐行证据支持单序列价格动量或波动率；选参仅使用开发或训练段，走步证据按轮保存，不混合角色和窗口。",
            )}
          </p>
          {parameters.retain_observations && !observationSupport && (
            <p role="status">
              {t("当前配置不支持逐行证据；请调整配置或取消保存，不会静默丢弃该选择。")}
            </p>
          )}
          <div className="workflow-actions">
            <button
              className="primary"
              type="submit"
              disabled={
                !variableReady ||
                !expressionSupported ||
                !taskService?.online ||
                !snapshot?.data?.online ||
                !series ||
                (filtered && selectionCount < 3) ||
                (parameters.retain_observations && !observationSupport)
              }
            >
              {pendingId ? t("确认提交状态") : t("开始分析")}
            </button>
          </div>
        </fieldset>
      </form>
      {submitted && (
        <p className="subtle" role="status">
          {t("任务已提交，可在任务中心查看进度。")}
        </p>
      )}
      <details>
        <summary>{t("计算说明")}</summary>
        <p>
          {factor === "term_structure"
            ? t(
                "期限结构只使用前一交易日已有的结算价，留出段可使用此前历史预热。未来标签不进入因子，跨分界标签剔除。留出评价需事先固定参数；反复查看留出结果后调参不能证明样本外有效。",
              )
            : t(
                "{factor}只使用过去价格，留出段可使用此前历史预热。未来标签不进入因子，跨分界标签剔除。留出评价需事先固定参数；反复查看留出结果后调参不能证明样本外有效。",
                { factor: t(factorNames[factor]) },
              )}
        </p>
      </details>
    </>
  );
}

/** Momentum on one contract's bars, read from the selected dataset. */
export function BarFactorForm({
  snapshot,
  query,
  busy,
  trade,
  navigate,
  run,
  variableDefinition,
  variableReady,
}: Pick<TerminalContext, "snapshot" | "query" | "busy" | "trade" | "navigate"> &
  VariableSelection & { run: Run }) {
  const datasets = snapshot?.datasets ?? [];
  const { alone, dominant, count } = selectedSeries(snapshot);
  // One series is studied over time, three or more against each other.
  const cross = count > 1;
  const [factor, setFactor] = useWorkspaceDraft<FactorKind>("factor:bars:factor", "momentum");
  // The term structure compares a dominant month with a later one: it is read
  // of dominant series alone, where the months after the dominant one exist.
  const unavailable =
    variableDefinition || factor !== "term_structure"
      ? null
      : alone.length
        ? t("期限结构只在主力连续上评价：请去掉单个合约，只保留主力连续。")
        : dominant.some(series => !series.terms)
          ? t(
              "所选主力连续里有的没有期限结构数据：主力月份之后还需要有已下载日线的月份，才能比较近远月。",
            )
          : null;
  return (
    <>
      <h3>{t("因子设置")}</h3>
      <DatasetPicker
        query={query}
        snapshot={snapshot}
        busy={busy}
        trade={trade}
        onDownload={() => navigate("workspace.history")}
      />
      <div className="research-fields">
        {!variableDefinition && (
          <label>
            {t("因子")}
            <select
              aria-label={t("因子")}
              value={factor}
              onChange={event => setFactor(event.target.value as FactorKind)}
            >
              {Object.entries(factorNames).map(([value, name]) => (
                <option key={value} value={value}>
                  {t(name)}
                </option>
              ))}
            </select>
          </label>
        )}
      </div>
      <p className="subtle">
        {t(
          !variableDefinition && factor === "term_structure"
            ? "期限结构 · 每个交易日开始时已知的年化近远月价差：前一交易日主力月份的结算价 ÷ 当日持仓最大的后续月份的结算价 − 1，再乘以 12 ÷ 两个月份相隔的月数；回看窗口内取均值。只在能做这个比较的交易日上评价。"
            : cross
              ? "多个合约 · 在每根 K 线上比较当时有数据的合约的因子值与未来收益"
              : dominant.length
                ? "主力连续 · 每个交易日取当时主力月份的 K 线，收盘价按换月比例调整到最新月份的水平"
                : "单合约 · 按 K 线收盘价计算",
        )}
      </p>
      {!variableDefinition && factor === "volatility" && (
        <p className="subtle">
          {t(
            "波动率 · 回看窗口内逐条收益率（相邻两条观测的收盘价之比减一）的样本标准差，回看窗口至少为 2。",
          )}
        </p>
      )}
      {unavailable && (
        <p role="alert" className="alert">
          {unavailable}
        </p>
      )}
      {count === 2 && (
        <p role="alert" className="alert">
          {t("比较合约至少需要 3 个；只分析一个合约时请只保留一个数据集。")}
        </p>
      )}
      <FactorParameters
        kind="bars"
        factor={factor}
        series={(count === 1 || count > 2) && !unavailable ? { kind: "bars" } : null}
        identity={JSON.stringify(datasets.map(item => item.revision))}
        observations={cross ? undefined : alone[0]?.count}
        selectionCount={count}
        variableDefinition={variableDefinition}
        variableReady={variableReady}
        snapshot={snapshot}
        busy={busy}
        run={run}
      />
    </>
  );
}

// A contract (SHFE/rb/2026-10) or a product (SHFE/rb) of the market-wide table.
const instrumentPattern = /^[A-Z]+\/[a-z]+(\/\d{4}-\d{2})?$/;
// As many series as one cross-section compares.
const maxInstruments = 100;

// What is asked of the daily table: instruments read from the same day go
// together. An exchange has no trading day before its first contract was
// listed, so its instruments are read from then on when the range begins
// earlier; the range itself must be stored whole.
export function tableRanges(instruments: string[], begin: string, products: TableProduct[]) {
  const opened = new Map<string, string>();
  for (const product of products) {
    const first = opened.get(product.exchange);
    if (!first || product.first_listed < first) opened.set(product.exchange, product.first_listed);
  }
  const ranges: { begin_day: string; instruments: string[] }[] = [];
  for (const name of instruments) {
    const first = opened.get(name.split("/")[0]) ?? "";
    const begin_day = first > begin ? first : begin;
    const range = ranges.find(item => item.begin_day === begin_day);
    if (range) range.instruments.push(name);
    else ranges.push({ begin_day, instruments: [name] });
  }
  return ranges;
}
/**
 * A factor on instruments of the market-wide daily table: a product is read
 * as its dominant series, a contract as its own closes. What is read is what
 * the store's current snapshot holds.
 */
export function TableFactorForm({
  snapshot,
  query,
  busy,
  navigate,
  run,
  variableDefinition,
  variableReady,
}: Pick<TerminalContext, "snapshot" | "query" | "busy" | "navigate"> &
  VariableSelection & { run: Run }) {
  const online = !!snapshot?.data?.online;
  const connection = snapshot?.data?.connection_id;
  const destination = JSON.stringify([connection, snapshot?.research_service?.connection_id]);
  const [held, setHeld] = useState<{ coverage: TableCoverage; products: TableProduct[] } | null>(
    null,
  );
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const ask = useRef(query);
  ask.current = query;
  // Replies of an earlier request, or to an earlier connection, are dropped.
  const sequence = useRef({ value: 0 });
  const load = useCallback(async () => {
    const generation = sequence.current;
    const current = ++generation.value;
    setLoading(true);
    setError("");
    try {
      const coverage = (await ask.current("data.tables.coverage", {})).table_coverage;
      if (!coverage) return;
      const lists = coverage.sources.find(source => source.table === "futures.contracts");
      const products =
        coverage.snapshot && lists
          ? ((
              await ask.current("data.tables.products", {
                snapshot: coverage.snapshot,
                contracts_source: lists.source,
              })
            ).table_products ?? [])
          : [];
      if (current === generation.value) setHeld({ coverage, products });
    } catch (e) {
      if (current === generation.value) setError(asDisplayError(e));
    } finally {
      if (current === generation.value) setLoading(false);
    }
  }, []);
  useEffect(() => {
    const generation = sequence.current;
    setHeld(null);
    if (online) void load();
    return () => {
      ++generation.value;
    };
  }, [online, connection, load]);
  const [factor, setFactor] = useWorkspaceDraft<FactorKind>("factor:tables:factor", "momentum");
  const [instruments, setInstruments] = useWorkspaceDraft<string[]>(
    `factor:tables:instruments:${destination}`,
    [],
  );
  const [range, setRange] = useWorkspaceDraft(`factor:tables:range:${destination}`, {
    begin: "",
    end: "",
  });
  const [entry, setEntry] = useState("");
  // The daily table that holds data, and the calendar of the same data source.
  const coverage = held?.coverage;
  const daily = dailyTableOf(coverage);
  const stored = daily?.stored ?? [];
  const exchanges = [...new Set(instruments.map(item => item.split("/")[0]))];
  const add = () => {
    const name = entry.trim();
    if (
      !instrumentPattern.test(name) ||
      instruments.includes(name) ||
      instruments.length >= maxInstruments
    )
      return;
    setInstruments([...instruments, name]);
    setEntry("");
  };
  // Products with a contract listed at some time in the range: one that begins
  // or ends inside it is compared on the days it has.
  const listed = (held?.products ?? [])
    .filter(
      product =>
        !!range.begin &&
        !!range.end &&
        product.first_listed <= range.end &&
        product.last_delisted >= range.begin,
    )
    .map(product => `${product.exchange}/${product.product}`)
    .filter(name => !instruments.includes(name));
  const addListed = () => setInstruments([...instruments, ...listed].slice(0, maxInstruments));
  const contracts = instruments.filter(item => item.split("/").length === 3);
  const ranges = tableRanges(instruments, range.begin, held?.products ?? []);
  // Exchanges read from the day they began, and those that begin after the range.
  const exchangesOf = (names: string[]) => [...new Set(names.map(name => name.split("/")[0]))];
  const later = ranges.filter(item => item.begin_day > range.begin);
  const absent = later
    .filter(item => !!range.end && item.begin_day > range.end)
    .flatMap(item => item.instruments);
  const unavailable =
    !variableDefinition && factor === "term_structure" && contracts.length
      ? t("期限结构只在品种的主力连续上评价：请去掉单个合约。")
      : absent.length
        ? t("{names} 所在的交易所在结束日期之后才有合约上市：请去掉它们。", {
            names: absent.join("、"),
          })
        : null;
  const complete =
    !!daily &&
    (instruments.length === 1 || instruments.length > 2) &&
    !!range.begin &&
    !!range.end &&
    range.begin <= range.end &&
    !unavailable;
  // A factor reads closes only: the contract list is not part of its request.
  const series: CommandParams["factor.submit"]["series"] | null = complete
    ? {
        kind: "tables",
        snapshot: daily.table.snapshot,
        daily_source: daily.table.daily_source,
        calendar_source: daily.table.calendar_source,
        end_day: range.end,
        ranges,
      }
    : null;
  return (
    <>
      <h3>{t("数据表因子设置")}</h3>
      <p className="subtle">
        {t(
          "从全市场日线表读取。品种按主力连续读取：按前一交易日的持仓量选月份、只向后换月，收盘价按换月比例调整到最新月份的水平，序列从范围内的第二个交易日开始；合约按它自己的收盘价读取。范围内有交易日没有入库时任务会被拒绝，不会跳过。",
        )}
      </p>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {held && !daily && <p role="status">{t("还没有入库的全市场日线。")}</p>}
      <div className="workflow-actions">
        <button disabled={busy} onClick={() => navigate("workspace.tables")}>
          {t("更新全市场数据")}
        </button>
        <button disabled={busy || loading || !online} onClick={() => void load()}>
          {t("刷新")}
        </button>
      </div>
      {daily && (
        <>
          <p className="subtle">
            {t("已入库的日线：{ranges}", {
              ranges: stored
                .filter(row => !exchanges.length || exchanges.includes(row.exchange))
                .map(row => `${row.exchange} ${row.first_period} — ${row.last_period}`)
                .join("；"),
            })}
          </p>
          <div className="research-fields">
            {!variableDefinition && (
              <label>
                {t("因子")}
                <select
                  aria-label={t("因子")}
                  value={factor}
                  onChange={event => setFactor(event.target.value as FactorKind)}
                >
                  {Object.entries(factorNames).map(([value, name]) => (
                    <option key={value} value={value}>
                      {t(name)}
                    </option>
                  ))}
                </select>
              </label>
            )}
            <label>
              {t("起始日期")}
              <input
                type="date"
                aria-label={t("起始日期")}
                max={range.end || undefined}
                value={range.begin}
                onChange={event => setRange({ ...range, begin: event.target.value })}
              />
            </label>
            <label>
              {t("结束日期")}
              <input
                type="date"
                aria-label={t("结束日期")}
                min={range.begin || undefined}
                value={range.end}
                onChange={event => setRange({ ...range, end: event.target.value })}
              />
            </label>
          </div>
          <div className="research-fields">
            <label>
              {t("品种或合约")}
              <input
                aria-label={t("品种或合约")}
                list="factor-table-products"
                placeholder={t("例如 SHFE/rb 或 SHFE/rb/2026-10")}
                value={entry}
                onChange={event => setEntry(event.target.value)}
                onKeyDown={event => {
                  if (event.key !== "Enter") return;
                  event.preventDefault();
                  add();
                }}
              />
              <datalist id="factor-table-products">
                {(held?.products ?? []).map(product => (
                  <option
                    key={`${product.exchange}/${product.product}`}
                    value={`${product.exchange}/${product.product}`}
                  >
                    {`${product.first_listed} — ${product.last_delisted}`}
                  </option>
                ))}
              </datalist>
            </label>
            <button
              type="button"
              disabled={
                busy ||
                !instrumentPattern.test(entry.trim()) ||
                instruments.includes(entry.trim()) ||
                instruments.length >= maxInstruments
              }
              onClick={add}
            >
              {t("添加")}
            </button>
            <button
              type="button"
              disabled={busy || !listed.length || instruments.length >= maxInstruments}
              onClick={addListed}
            >
              {t("添加范围内有合约在市的全部品种")}
            </button>
          </div>
          <ul className="research-instruments" aria-label={t("已选的品种和合约")}>
            {instruments.map(item => (
              <li key={item}>
                {item}
                <button
                  type="button"
                  disabled={busy}
                  aria-label={t("移除 {name}", { name: item })}
                  onClick={() => setInstruments(instruments.filter(other => other !== item))}
                >
                  ×
                </button>
              </li>
            ))}
          </ul>
          <p className="subtle">
            {t(
              "一个品种或合约按时间评价；3 到 100 个在每个交易日上相互比较，只比较当天有数据的那些。按当前快照 {id} 读取，之后入库的数据不影响这次任务。",
              { id: coverage?.snapshot.slice(0, 12) ?? "" },
            )}
          </p>
          {!!range.begin && later.length > 0 && !absent.length && (
            <p role="status">
              {t("交易所在起始日期之后才有合约上市的，它的品种从第一个上市日起读取：{list}。", {
                list: later
                  .map(item => `${exchangesOf(item.instruments).join("、")} ${item.begin_day}`)
                  .join("；"),
              })}
            </p>
          )}
          {instruments.length > 2 && (
            <p className="subtle">
              {t(
                "可启用历史流动性筛选，按此前已知的成交量、持仓量和连续观测数量决定每日资格；不用整个研究区间的成交结果提前剔除品种。",
              )}
            </p>
          )}
          {instruments.length === 2 && (
            <p role="alert" className="alert">
              {t("截面比较至少需要 3 个品种或合约；只分析一个时请只保留一个。")}
            </p>
          )}
          {unavailable && (
            <p role="alert" className="alert">
              {unavailable}
            </p>
          )}
        </>
      )}
      <FactorParameters
        kind="tables"
        selectionCount={instruments.length}
        variableDefinition={variableDefinition}
        variableReady={variableReady}
        factor={factor}
        series={series}
        identity={JSON.stringify(series)}
        snapshot={snapshot}
        busy={busy}
        run={run}
      />
    </>
  );
}
