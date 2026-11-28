// Research form state, scope matching, Protobuf request builders and record names.
import type { MessageInitShape } from "@bufbuild/protobuf";
import { Period } from "../../src/generated/asterion/common_pb";
import { type MessageValues, translate } from "../../src/i18n";
import { protocolPeriod } from "../../src/ui/periods";
import { RunState } from "../../src/generated/asterion/lab_pb";
import type {
  BacktestSchema,
  EvaluateFactorSchema,
  FactorEntry,
  FactorResult,
  Parameter,
  RunDetail,
  RunSummary,
} from "../../src/generated/asterion/lab_pb";
import type { ParameterValueSchema } from "../../src/generated/asterion/strategy_pb";
import { type ConditionSet, readConditionSet, sameSymbolList, symbolsOf } from "./research-storage";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
type ValueInit = MessageInitShape<typeof ParameterValueSchema>;
export type RunInit = MessageInitShape<typeof BacktestSchema>;
export type FactorInit = MessageInitShape<typeof EvaluateFactorSchema>;
const decimal = (text: string) => ({ text });

// A run's state as the pages name it: running, succeeded, failed, stopped or
// interrupted; empty for a sweep's run that has not started.
export const runState = (state: RunState) =>
  state === RunState.UNSPECIFIED ? "" : RunState[state].toLowerCase();

// A parameter's type, as its default value shows it.
function parameterKind(parameter: Parameter) {
  return ({ flag: "bool", integer: "int", number: "float", text: "str" } as Record<string, string>)[
    parameter.default?.value.case ?? "text"
  ]!;
}

export type RunForm = {
  symbols: string;
  multiSymbolMode?: "separate" | "portfolio";
  begin: string;
  end: string;
  // Daily bars ("0") or bars of this many minutes.
  minutes: string;
  params: Record<string, string>;
  deposit: string;
  slippage: string;
  feeMultiplier: string;
  maxOrderLots: string;
  maxContractLots: string;
  maxGrossLots: string;
  maxWorkingOrders: string;
  maxGrossWeight: string;
  maxNetWeight: string;
  maxMarginRatio: string;
  minRebalanceWeight: string;
  annualPeriods: string;
  zeroRiskFree: boolean;
  validation: "none" | "holdout" | "rolling";
  holdoutDay: string;
  trainingDays: string;
  validationDays: string;
  embargoDays?: string;
  rollingObjective?: "sharpe" | "dsr" | "calmar" | "total_return";
  sweepMode?: "grid" | "lhs" | "bayes";
  maxTrials?: string;
  processes: string;
};

export const defaultProcesses = () => String(Math.max(1, (navigator.hardwareConcurrency || 2) - 1));

export function defaultForm(): RunForm {
  return {
    symbols: "",
    begin: "",
    end: "",
    minutes: "0",
    params: {},
    deposit: "1000000",
    slippage: "1",
    feeMultiplier: "1",
    maxOrderLots: "100",
    maxContractLots: "500",
    maxGrossLots: "2000",
    maxWorkingOrders: "64",
    maxGrossWeight: "1",
    maxNetWeight: "1",
    maxMarginRatio: "",
    minRebalanceWeight: "",
    annualPeriods: "252",
    zeroRiskFree: true,
    validation: "none",
    holdoutDay: "",
    trainingDays: "60",
    validationDays: "20",
    embargoDays: "0",
    rollingObjective: "sharpe",
    sweepMode: "grid",
    maxTrials: "24",
    processes: defaultProcesses(),
  };
}

const conditionsKey = "asterion.research.scope";
const scopeFields = [
  "symbols",
  "begin",
  "end",
  "minutes",
  "deposit",
  "slippage",
  "feeMultiplier",
  "maxOrderLots",
  "maxContractLots",
  "maxGrossLots",
  "maxWorkingOrders",
  "maxGrossWeight",
  "maxNetWeight",
  "maxMarginRatio",
  "minRebalanceWeight",
  "annualPeriods",
  "zeroRiskFree",
  "holdoutDay",
] as const;

export type ScopeFields = Pick<RunForm, (typeof scopeFields)[number]>;
export type RunConditions = ConditionSet<ScopeFields>;

export const scopeOf = (form: RunForm) =>
  Object.fromEntries(scopeFields.map(field => [field, form[field]])) as ScopeFields;

export const readConditions = () => readConditionSet(conditionsKey, () => scopeOf(defaultForm()));

export function saveConditions(conditions: RunConditions) {
  localStorage.setItem(conditionsKey, JSON.stringify(conditions));
}

// The form, on the working draft or active preset from the last session.
export function savedForm(): RunForm {
  const { active, current, items } = readConditions();
  return {
    ...defaultForm(),
    ...(current ?? items.find(item => item.name === active)?.fields),
  };
}

// Whether two sets of settings say the same: "1000000.00" is 1000000, blank is 0.
export function sameScope(a: ScopeFields, b: ScopeFields) {
  return scopeFields.every(field => {
    if (field === "symbols") return sameSymbolList(a.symbols, b.symbols);
    if (field === "feeMultiplier")
      return Number(a.feeMultiplier || 1) === Number(b.feeMultiplier || 1);
    const [x, y] = [a[field], b[field]];
    return (
      x === y ||
      (typeof x === "string" && typeof y === "string" && Number(x || 0) === Number(y || 0))
    );
  });
}

// A blank day means `stored`; a day not yet known matches any.
const sameDay = (value: string, stored: string, actual: string) =>
  !/^\d/.test(value || stored) || (value || stored) === actual;

// Whether a run was measured on the form's settings. `begin` and `end` are what
// blank days resolve to; a day not yet known matches any.
export function onScope(form: RunForm, run: RunSummary, begin: string, end: string) {
  const settings = run.settings;
  if (!settings) return false;
  const symbols = symbolsOf(form.symbols);
  const same = (text: string, value?: { text: string }) =>
    Number(text || 0) === Number(value?.text || 0);
  return (
    symbols.length === run.symbols.length &&
    symbols.every(symbol => run.symbols.includes(symbol)) &&
    sameDay(form.begin, begin, run.firstDay) &&
    sameDay(form.end, end, run.lastDay) &&
    protocolPeriod(form.minutes === "0" ? "day" : Number(form.minutes)) === run.period &&
    same(form.deposit, settings.deposit) &&
    Number(form.slippage) === settings.slippageTicks &&
    Number(form.feeMultiplier || 1) === Number(settings.feeMultiplier?.text || 1) &&
    same(form.maxOrderLots, settings.limits?.maxOrderLots) &&
    same(form.maxContractLots, settings.limits?.maxContractLots) &&
    same(form.maxGrossLots, settings.limits?.maxGrossLots) &&
    Number(form.maxWorkingOrders) === settings.limits?.maxWorkingOrders &&
    same(form.maxGrossWeight, settings.maxGrossWeight) &&
    same(form.maxNetWeight, settings.maxNetWeight) &&
    same(form.maxMarginRatio, settings.maxMarginRatio) &&
    same(form.minRebalanceWeight, settings.minRebalanceWeight) &&
    (Number(form.annualPeriods) || 0) === settings.periodsPerYear &&
    form.zeroRiskFree === settings.zeroRiskFreeRate &&
    form.holdoutDay === settings.holdoutDay
  );
}

// One field's values: comma-separated, each of the parameter's kind. More
// than one makes the run a sweep.
export function parameterValues(parameter: Parameter, text: string): ValueInit[] {
  const items = text
    .split(/[,，]/)
    .map(item => item.trim())
    .filter(Boolean);
  const name = parameter.name;
  if (!items.length) throw new Error(t("参数 {name} 没有值", { name }));
  return items.map(item => {
    switch (parameterKind(parameter)) {
      case "int":
        if (!/^-?\d+$/.test(item))
          throw new Error(t("参数 {name} 的值 {item} 不是整数", { name, item }));
        return { value: { case: "integer", value: BigInt(item) } };
      case "float": {
        const value = Number(item);
        if (!Number.isFinite(value))
          throw new Error(t("参数 {name} 的值 {item} 不是数", { name, item }));
        return { value: { case: "number", value } };
      }
      case "bool":
        if (item !== "true" && item !== "false")
          throw new Error(t("参数 {name} 的值只能是 true 或 false", { name }));
        return { value: { case: "flag", value: item === "true" } };
      default:
        return { value: { case: "text", value: item } };
    }
  });
}

export function defaultText(parameter: Parameter) {
  const value = parameter.default?.value;
  return value?.case === undefined ? "" : String(value.value);
}

export function parameterText(value: { value: { case?: string; value?: unknown } } | undefined) {
  return value?.value.case === undefined ? "" : String(value.value.value);
}

// A batch's run names its strategy among its swept values, before it has started.
export const batchKey = "__strategy__";
export const slippageKey = "__slippage__";
export const batchStrategy = (run: RunSummary) => parameterText(run.params[batchKey]);

export function slippageStressBase(detail: RunDetail, fallbackRepository: string): RunInit {
  const rec = JSON.parse(detail.record) as {
    strategy?: { id?: string; repository?: string };
    params?: Record<string, unknown>;
    data?: { symbols?: string[]; begin_day?: string; end_day?: string; bars?: { period?: string } };
    costs?: { deposit?: string; slippage_ticks?: number; fee_multiplier?: string | number };
    risk?: Record<string, string | number>;
    statistics?: {
      annual_periods?: number | null;
      zero_risk_free?: boolean;
      holdout_day?: string;
      rolling?: unknown;
    };
  };
  if (rec.statistics?.rolling) throw new Error(t("滑点压力测试需要选择单次候选回测"));
  const period = rec.data?.bars?.period ?? "day";
  const minutes = period === "day" ? 0 : Number(period.replace(/m$/, ""));
  const params: Record<string, ValueInit> = {};
  for (const [k, v] of Object.entries(rec.params ?? {})) {
    if (typeof v === "boolean") params[k] = { value: { case: "flag", value: v } };
    else if (typeof v === "number" && Number.isInteger(v))
      params[k] = { value: { case: "integer", value: BigInt(v) } };
    else if (typeof v === "number") params[k] = { value: { case: "number", value: v } };
    else if (typeof v === "string") params[k] = { value: { case: "text", value: v } };
  }
  const risk = rec.risk ?? {};
  return {
    id: detail.summary?.id ?? "",
    repository: rec.strategy?.repository || fallbackRepository,
    strategy: rec.strategy?.id || detail.summary?.strategy || "",
    params,
    symbols: rec.data?.symbols ?? detail.summary?.symbols ?? [],
    firstDay: rec.data?.begin_day ?? detail.summary?.firstDay ?? "",
    lastDay: rec.data?.end_day ?? detail.summary?.lastDay ?? "",
    period: protocolPeriod(minutes || "day"),
    deposit: decimal(String(rec.costs?.deposit ?? "1000000")),
    slippageTicks: Number(rec.costs?.slippage_ticks ?? 0),
    feeMultiplier: decimal(String(rec.costs?.fee_multiplier ?? "1")),
    limits: {
      maxOrderLots: decimal(String(risk.max_order_lots ?? "100")),
      maxContractLots: decimal(String(risk.max_contract_lots ?? "500")),
      maxGrossLots: decimal(String(risk.max_gross_lots ?? "2000")),
      maxWorkingOrders: Number(risk.max_working_orders ?? 64),
    },
    maxGrossWeight: decimal(String(risk.max_gross_weight ?? "1")),
    maxNetWeight: decimal(String(risk.max_net_weight ?? "1")),
    maxMarginRatio: decimal(String(risk.max_margin_ratio ?? "0")),
    minRebalanceWeight: decimal(String(risk.min_rebalance_weight ?? "0")),
    periodsPerYear: Number(rec.statistics?.annual_periods ?? 252),
    zeroRiskFreeRate: Boolean(rec.statistics?.zero_risk_free ?? true),
    holdoutDay: rec.statistics?.holdout_day ?? "",
  };
}

export function formFromRun(detail: RunDetail): { strategy: string; form: RunForm } {
  const base = defaultForm();
  const record = detail.record ? JSON.parse(detail.record) : {};
  const summary = detail.summary;
  const strategy = (record.strategy?.id as string | undefined) ?? summary?.strategy ?? "";
  const params =
    record.params && typeof record.params === "object"
      ? Object.fromEntries(
          Object.entries(record.params as Record<string, unknown>).map(([k, v]) => [k, String(v)]),
        )
      : Object.fromEntries(
          Object.entries(summary?.params ?? {}).map(([k, v]) => [k, parameterText(v)]),
        );
  const period = record.data?.bars?.period as string | undefined;
  const minutes = period
    ? period === "day"
      ? "0"
      : period.replace("m", "")
    : summary?.period && summary.period !== Period.DAY
      ? (Period[summary.period]?.replace("MINUTES_", "") ?? "0")
      : "0";
  const lots = (value: unknown) => {
    const text = value === undefined || value === null ? "" : String(value);
    return text === "0" ? "" : text;
  };
  const holdoutDay = String(record.statistics?.holdout_day ?? "");
  const rolling = record.statistics?.rolling as
    | {
        training_days?: number;
        validation_days?: number;
        embargo_days?: number;
        objective?: "sharpe" | "dsr" | "calmar" | "total_return";
      }
    | undefined;
  const symbolList = (record.data?.symbols as string[] | undefined) ?? summary?.symbols ?? [];
  return {
    strategy,
    form: {
      ...base,
      symbols: symbolList.join(", "),
      multiSymbolMode: symbolList.length > 1 ? "portfolio" : undefined,
      begin: String(record.data?.begin_day ?? summary?.firstDay ?? ""),
      end: String(record.data?.end_day ?? summary?.lastDay ?? ""),
      minutes,
      params,
      deposit: String(record.costs?.deposit ?? base.deposit),
      slippage: String(record.costs?.slippage_ticks ?? base.slippage),
      feeMultiplier: String(record.costs?.fee_multiplier ?? base.feeMultiplier),
      maxOrderLots: lots(record.risk?.max_order_lots),
      maxContractLots: lots(record.risk?.max_contract_lots),
      maxGrossLots: lots(record.risk?.max_gross_lots),
      maxWorkingOrders: String(record.risk?.max_working_orders ?? base.maxWorkingOrders),
      maxGrossWeight: String(record.risk?.max_gross_weight ?? base.maxGrossWeight),
      maxNetWeight: String(record.risk?.max_net_weight ?? base.maxNetWeight),
      maxMarginRatio: lots(record.risk?.max_margin_ratio),
      minRebalanceWeight: lots(record.risk?.min_rebalance_weight),
      annualPeriods:
        record.statistics?.annual_periods !== undefined && record.statistics.annual_periods !== null
          ? String(record.statistics.annual_periods)
          : base.annualPeriods,
      zeroRiskFree:
        typeof record.statistics?.zero_risk_free === "boolean"
          ? record.statistics.zero_risk_free
          : base.zeroRiskFree,
      validation: holdoutDay ? "holdout" : rolling ? "rolling" : "none",
      holdoutDay,
      trainingDays:
        rolling?.training_days !== undefined ? String(rolling.training_days) : base.trainingDays,
      validationDays:
        rolling?.validation_days !== undefined
          ? String(rolling.validation_days)
          : base.validationDays,
      embargoDays:
        rolling?.embargo_days !== undefined ? String(rolling.embargo_days) : base.embargoDays,
      rollingObjective: rolling?.objective ?? base.rollingObjective,
    },
  };
}

// The run the form describes and, where a field has several values, the
// values the sweep takes; with rolling validation the sweep is required.
export function runRequest(
  form: RunForm,
  repository: string,
  strategy: string,
  parameters: readonly Parameter[],
  id: string,
) {
  const symbols = symbolsOf(form.symbols);
  if (!symbols.length) throw new Error(t("请填写品种"));
  const swept: { name: string; values: ValueInit[] }[] = [];
  const params: Record<string, ValueInit> = {};
  for (const parameter of parameters) {
    const values = parameterValues(
      parameter,
      form.params[parameter.name] ?? defaultText(parameter),
    );
    if (values.length > 1) swept.push({ name: parameter.name, values });
    else params[parameter.name] = values[0];
  }
  const run: RunInit = {
    id,
    repository,
    strategy,
    params,
    symbols,
    firstDay: form.begin,
    lastDay: form.end,
    period: protocolPeriod(form.minutes === "0" ? "day" : Number(form.minutes)),
    deposit: decimal(form.deposit),
    slippageTicks: Number(form.slippage),
    feeMultiplier: form.feeMultiplier.trim() ? decimal(form.feeMultiplier) : decimal("1"),
    limits: {
      maxOrderLots: decimal(form.maxOrderLots),
      maxGrossLots: decimal(form.maxGrossLots),
      maxContractLots: decimal(form.maxContractLots),
      maxWorkingOrders: Number(form.maxWorkingOrders),
    },
    maxGrossWeight: decimal(form.maxGrossWeight),
    maxNetWeight: decimal(form.maxNetWeight),
    maxMarginRatio: form.maxMarginRatio.trim() ? decimal(form.maxMarginRatio) : undefined,
    minRebalanceWeight: form.minRebalanceWeight.trim()
      ? decimal(form.minRebalanceWeight)
      : undefined,
    periodsPerYear: Number(form.annualPeriods) || 0,
    zeroRiskFreeRate: form.zeroRiskFree,
    holdoutDay: form.validation === "holdout" ? form.holdoutDay : "",
  };
  return { run, swept };
}

export type FactorForm = {
  symbols: string;
  begin: string;
  end: string;
  // Daily bars ("0") or bars of this many minutes.
  minutes?: string;
  params: Record<string, string>;
  horizons: string;
  groups: string;
  standardize?: "" | "zscore" | "rank";
  extraFactors?: string[];
  orthogonalizeAgainst?: string[];
  combination?: "equal" | "ic_weighted";
  holdoutDay?: string;
  returnMode?: "open" | "close";
  winsorize?: boolean;
  neutralize?: "" | "sector";
  minVolume?: string;
  minOpenInterest?: string;
  volAdjust?: boolean;
  costBps?: string;
};

export function defaultFactorForm(): FactorForm {
  return {
    symbols: "",
    begin: "",
    end: "",
    minutes: "0",
    params: {},
    horizons: "1, 2, 3, 5, 10, 20",
    groups: "5",
    standardize: "",
    extraFactors: [],
    orthogonalizeAgainst: [],
    combination: "equal",
    holdoutDay: "",
    returnMode: "open",
    winsorize: false,
    neutralize: "",
    minVolume: "",
    minOpenInterest: "",
    volAdjust: false,
    costBps: "",
  };
}

const factorConditionsKey = "asterion.research.factor-scope";
type FactorScopeField =
  | "symbols"
  | "begin"
  | "end"
  | "minutes"
  | "horizons"
  | "groups"
  | "standardize"
  | "holdoutDay"
  | "returnMode"
  | "winsorize"
  | "neutralize"
  | "minVolume"
  | "minOpenInterest"
  | "volAdjust"
  | "costBps";
export type FactorScope = Required<Pick<FactorForm, FactorScopeField>>;

export const factorScopeOf = (form: FactorForm): FactorScope => {
  const defaults = defaultFactorForm();
  return {
    symbols: form.symbols ?? defaults.symbols,
    begin: form.begin ?? defaults.begin,
    end: form.end ?? defaults.end,
    minutes: form.minutes ?? defaults.minutes!,
    horizons: form.horizons ?? defaults.horizons,
    groups: form.groups ?? defaults.groups,
    standardize: form.standardize ?? defaults.standardize!,
    holdoutDay: form.holdoutDay ?? defaults.holdoutDay!,
    returnMode: form.returnMode ?? defaults.returnMode!,
    winsorize: form.winsorize ?? defaults.winsorize!,
    neutralize: form.neutralize ?? defaults.neutralize!,
    minVolume: form.minVolume ?? defaults.minVolume!,
    minOpenInterest: form.minOpenInterest ?? defaults.minOpenInterest!,
    volAdjust: form.volAdjust ?? defaults.volAdjust!,
    costBps: form.costBps ?? defaults.costBps!,
  };
};

export const readFactorConditions = () =>
  readConditionSet(factorConditionsKey, () => factorScopeOf(defaultFactorForm()));

export function saveFactorConditions(conditions: ConditionSet<FactorScope>) {
  localStorage.setItem(factorConditionsKey, JSON.stringify(conditions));
}

export function savedFactorForm(): FactorForm {
  const { active, current, items } = readFactorConditions();
  return {
    ...defaultFactorForm(),
    ...(current ?? items.find(item => item.name === active)?.fields),
  };
}

export function sameFactorScope(a: FactorScope, b: FactorScope) {
  const sameNum = (x: string, y: string) => Number(x.trim() || 0) === Number(y.trim() || 0);
  const sameList = (x: string, y: string) => {
    const [p, q] = [symbolsOf(x).map(Number), symbolsOf(y).map(Number)];
    return p.length === q.length && p.every((value, idx) => value === q[idx]);
  };
  return (
    sameSymbolList(a.symbols, b.symbols) &&
    a.begin.trim() === b.begin.trim() &&
    a.end.trim() === b.end.trim() &&
    sameNum(a.minutes, b.minutes) &&
    sameList(a.horizons, b.horizons) &&
    sameNum(a.groups, b.groups) &&
    a.standardize === b.standardize &&
    a.holdoutDay.trim() === b.holdoutDay.trim() &&
    a.returnMode === b.returnMode &&
    a.winsorize === b.winsorize &&
    a.neutralize === b.neutralize &&
    sameNum(a.minVolume, b.minVolume) &&
    sameNum(a.minOpenInterest, b.minOpenInterest) &&
    a.volAdjust === b.volAdjust &&
    sameNum(a.costBps, b.costBps)
  );
}

// Whether an evaluation was made on the form's settings. `begin` and `end` are
// what blank days resolve to.
export function onFactorScope(form: FactorForm, result: FactorResult, begin: string, end: string) {
  const evaluation = result.evaluation;
  if (!evaluation) return false;
  const symbols = symbolsOf(form.symbols);
  const horizons = symbolsOf(form.horizons).map(Number);
  const sameNumber = (text: string | undefined, value: number) =>
    Number((text ?? "").trim() || 0) === (value || 0);
  const formPeriod = protocolPeriod(
    !form.minutes || form.minutes === "0" ? "day" : Number(form.minutes),
  );
  const evalPeriod = evaluation.period || Period.DAY;
  return (
    symbols.length === evaluation.symbols.length &&
    symbols.every(symbol => evaluation.symbols.includes(symbol)) &&
    sameDay(form.begin, begin, evaluation.firstDay) &&
    sameDay(form.end, end, evaluation.lastDay) &&
    formPeriod === evalPeriod &&
    horizons.length === evaluation.horizons.length &&
    horizons.every((horizon, index) => horizon === evaluation.horizons[index]) &&
    Number(form.groups) === evaluation.groups &&
    (form.standardize ?? "") === evaluation.standardize &&
    (form.holdoutDay ?? "").trim() === evaluation.holdoutDay &&
    (form.returnMode ?? "open") === (evaluation.returnMode || "close") &&
    Boolean(form.winsorize) === evaluation.winsorize &&
    (form.neutralize ?? "") === evaluation.neutralize &&
    sameNumber(form.minVolume, evaluation.minVolume) &&
    sameNumber(form.minOpenInterest, evaluation.minOpenInterest) &&
    Boolean(form.volAdjust) === evaluation.volAdjust &&
    sameNumber(form.costBps, evaluation.costBps)
  );
}

// Runs, sweeps and factor results are named by their module and the time they
// were asked for, to the millisecond so that two asked for in one second differ.
export function identity(code: string, suffix = "") {
  const stamp = new Date()
    .toISOString()
    .replace(/[-:T.]/g, "")
    .slice(0, 17);
  const tail = suffix ? `-${suffix.replace(/[^A-Za-z0-9_-]/g, "-")}` : "";
  const prefix = code
    .split(".")[0]
    .replace(/[^A-Za-z0-9_-]/g, "-")
    .slice(0, Math.max(1, 45 - tail.length));
  return `${prefix}-${stamp}${tail}`;
}

// An id as shown: its module and that time in Beijing time, to the second.
const RECORD_ID = /^(.*)-(\d{4})(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)\d{3}(.*)$/;

// When a record was made, from its id.
export function recordTime(id: string) {
  const match = RECORD_ID.exec(id);
  if (!match) return id;
  const [year, month, day, hour, minute, second] = match.slice(2, 8).map(Number);
  return new Date(Date.UTC(year, month - 1, day, hour, minute, second))
    .toLocaleString("sv-SE", { timeZone: "Asia/Shanghai", hour12: false })
    .slice(5);
}

export function recordName(id: string, name?: string) {
  const match = RECORD_ID.exec(id);
  return match ? `${name ?? match[1]} ${recordTime(id)}${match[8]}` : id;
}

// A run's label, or its strategy and time; only a sweep's runs need their index.
export function runName(run: Pick<RunSummary, "id" | "label" | "strategy">) {
  if (run.label) return run.label;
  const [module, name] = run.strategy.split(".");
  if (!name || !RECORD_ID.test(run.id)) return recordName(run.id);
  return run.id.startsWith(`${module}-`)
    ? recordName(run.id, name)
    : `${name} ${recordTime(run.id)}`;
}

export function factorRequest(
  form: FactorForm,
  repository: string,
  factor: FactorEntry,
  options?: { useDefaults?: boolean; extraFactors?: readonly string[] },
) {
  const symbols = symbolsOf(form.symbols);
  if (!symbols.length) throw new Error(t("请填写品种"));
  const params: Record<string, ValueInit> = {};
  for (const parameter of factor.parameters) {
    const raw = options?.useDefaults
      ? defaultText(parameter)
      : (form.params[parameter.name] ?? defaultText(parameter));
    const values = parameterValues(parameter, raw);
    if (values.length > 1)
      throw new Error(t("因子参数 {name} 只能取一个值", { name: parameter.name }));
    params[parameter.name] = values[0];
  }
  const horizons = symbolsOf(form.horizons).map(Number);
  if (
    !horizons.length ||
    horizons.some(value => !Number.isInteger(value) || value < 1) ||
    new Set(horizons).size !== horizons.length
  )
    throw new Error(t("持有期是互不相同的正整数"));
  const groups = Number(form.groups);
  if (!Number.isInteger(groups) || groups < 2) throw new Error(t("分组数至少为 2"));
  const nonNegative = (label: string, text: string | undefined) => {
    const trimmed = (text ?? "").trim();
    if (!trimmed) return 0;
    const value = Number(trimmed);
    if (!Number.isFinite(value) || value < 0)
      throw new Error(t("{label} 必须为非负数", { label: t(label) }));
    return value;
  };
  const minVolume = nonNegative("最低日均成交量", form.minVolume);
  const minOpenInterest = nonNegative("最低日均持仓量", form.minOpenInterest);
  const costBps = nonNegative("单边成本（bps）", form.costBps);
  const extras = (options?.extraFactors ?? form.extraFactors ?? []).filter(id => id !== factor.id);
  const controls = (form.orthogonalizeAgainst ?? []).filter(
    id => id !== factor.id && !extras.includes(id),
  );
  const request: FactorInit = {
    repository,
    factor: factor.id,
    params,
    symbols,
    firstDay: form.begin,
    lastDay: form.end,
    horizons,
    groups,
    standardize: form.standardize ?? "",
    extraFactors: extras.map(id => ({ factor: id, params: {} })),
    combination: extras.length ? (form.combination ?? "equal") : "",
    holdoutDay: form.holdoutDay?.trim() ?? "",
    returnMode: form.returnMode ?? "open",
    winsorize: Boolean(form.winsorize),
    neutralize: form.neutralize ?? "",
    minVolume,
    minOpenInterest,
    volAdjust: Boolean(form.volAdjust),
    costBps,
    orthogonalizeAgainst: controls.map(id => ({ factor: id, params: {} })),
    period: protocolPeriod(!form.minutes || form.minutes === "0" ? "day" : Number(form.minutes)),
  };
  return request;
}
