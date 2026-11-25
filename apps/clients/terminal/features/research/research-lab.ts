// What the research page computes without the lab: parameter values from the
// form, the request for a run, a sweep or a factor's evaluation, and figures
// read from a run record.
import type { MessageInitShape } from "@bufbuild/protobuf";
import { Period } from "../../src/generated/asterion/common_pb";
import { type MessageValues, translate } from "../../src/i18n";
import { protocolPeriod } from "../../src/ui/periods";
import { RunState } from "../../src/generated/asterion/lab_pb";
import type {
  BacktestSchema,
  EvaluateFactorSchema,
  FactorDay,
  FactorEntry,
  FactorResult,
  Parameter,
  RunDay,
  RunDetail,
  RunSummary,
} from "../../src/generated/asterion/lab_pb";

import type { ParameterValueSchema } from "../../src/generated/asterion/strategy_pb";

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
// Saved benchmark presets plus the current ad-hoc working draft.
export type ConditionSet<F> = {
  active: string;
  current?: F;
  items: { name: string; fields: F }[];
};
export type RunConditions = ConditionSet<ScopeFields>;

export const scopeOf = (form: RunForm) =>
  Object.fromEntries(scopeFields.map(field => [field, form[field]])) as ScopeFields;

function mergeConditionFields<F extends object>(
  defaults: () => F,
  raw: Record<string, unknown> | undefined,
): F {
  const fields = defaults() as Record<string, unknown>;
  for (const field of Object.keys(fields))
    if (typeof raw?.[field] === typeof fields[field]) fields[field] = raw![field];
  return fields as F;
}

function readConditionSet<F extends object>(key: string, defaults: () => F): ConditionSet<F> {
  try {
    const saved = JSON.parse(localStorage.getItem(key) ?? "");
    const items: ConditionSet<F>["items"] = Array.isArray(saved.items)
      ? saved.items
          .filter((item: { name?: unknown }) => item?.name && typeof item.name === "string")
          .map((item: { name: string; fields?: Record<string, unknown> }) => ({
            name: item.name,
            fields: mergeConditionFields(defaults, item.fields),
          }))
      : [];
    const active =
      typeof saved.active === "string" && items.some(item => item.name === saved.active)
        ? saved.active
        : "";
    const current =
      saved.current && typeof saved.current === "object"
        ? mergeConditionFields(defaults, saved.current as Record<string, unknown>)
        : active
          ? items.find(item => item.name === active)?.fields
          : undefined;
    return { active, current, items };
  } catch {
    // Nothing saved yet, or a damaged entry.
    return { active: "", items: [] };
  }
}

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

export function sameSymbolList(a: string, b: string) {
  const [x, y] = [symbolsOf(a), symbolsOf(b)];
  return x.length === y.length && x.every(symbol => y.includes(symbol));
}

// Whether two sets of settings say the same: "1000000.00" is 1000000, blank is 0.
export function sameScope(a: ScopeFields, b: ScopeFields) {
  return scopeFields.every(field => {
    if (field === "symbols") return sameSymbolList(a.symbols, b.symbols);
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

export function symbolsOf(text: string) {
  return text
    .split(/[\s,，]+/)
    .map(item => item.trim())
    .filter(Boolean);
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
export const batchStrategy = (run: RunSummary) => parameterText(run.params[batchKey]);

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
    { training_days?: number; validation_days?: number } | undefined;
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

// Each month's return from the equity it began with: the last settlement of
// the month before, or the deposit.
export function monthlyReturns(deposit: string, days: readonly RunDay[]) {
  const months: { month: string; value: number }[] = [];
  let opening = Number(deposit);
  for (let i = 0; i < days.length; i++) {
    const month = days[i].tradingDay.slice(0, 7);
    if (i + 1 < days.length && days[i + 1].tradingDay.slice(0, 7) === month) continue;
    const closing = Number(days[i].equity?.text);
    if (opening > 0 && Number.isFinite(closing)) {
      months.push({ month, value: Math.max(-1, closing / opening - 1) });
    }
    opening = closing;
  }
  return months;
}

// A sweep over two parameters as a grid of one metric; null where a
// combination has no value.
export function heatmap(
  runs: readonly { params: Record<string, string>; metric: number | null }[],
  row: string,
  column: string,
) {
  const sorted = (name: string) =>
    [...new Set(runs.map(run => run.params[name]))].sort(
      (a, b) => Number(a) - Number(b) || a.localeCompare(b),
    );
  const rows = sorted(row),
    columns = sorted(column);
  const cells = rows.map(r =>
    columns.map(
      c => runs.find(run => run.params[row] === r && run.params[column] === c)?.metric ?? null,
    ),
  );
  return { rows, columns, cells };
}

export type FactorForm = {
  symbols: string;
  begin: string;
  end: string;
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

// Shared symbol universes reused across StrategyLab and FactorLab.
export type Universe = { name: string; symbols: string };
const universesKey = "asterion.research.universes";
const universeListeners = new Set<() => void>();

export function readUniverses(): Universe[] {
  try {
    const parsed = JSON.parse(localStorage.getItem(universesKey) ?? "");
    if (!Array.isArray(parsed)) return [];
    return parsed.filter(
      (item): item is Universe =>
        Boolean(item) &&
        typeof item.name === "string" &&
        Boolean(item.name.trim()) &&
        typeof item.symbols === "string" &&
        symbolsOf(item.symbols).length > 0,
    );
  } catch {
    return [];
  }
}

export function saveUniverses(items: Universe[]) {
  localStorage.setItem(universesKey, JSON.stringify(items));
  universeListeners.forEach(listener => listener());
}

export function subscribeUniverses(listener: () => void) {
  universeListeners.add(listener);
  return () => {
    universeListeners.delete(listener);
  };
}

type SectorId =
  "black" | "nonferrous" | "precious" | "energy_chem" | "oilseeds" | "agriculture" | "financial";

// Mirrors asterion_strategy.PRODUCT_SECTORS so UI universe pools match backend neutralization.
const PRODUCT_SECTORS: Record<string, SectorId> = Object.fromEntries([
  ...["rb", "hc", "wr", "ss", "i", "j", "jm", "sf", "sm", "zc", "tc"].map(p => [p, "black"]),
  ...["cu", "bc", "al", "ad", "ao", "zn", "pb", "ni", "sn", "si", "ps", "lc"].map(p => [
    p,
    "nonferrous",
  ]),
  ...["au", "ag", "pt", "pd"].map(p => [p, "precious"]),
  ...[
    "sc",
    "fu",
    "lu",
    "bu",
    "ru",
    "nr",
    "br",
    "ta",
    "px",
    "pf",
    "pr",
    "pl",
    "ma",
    "me",
    "eg",
    "eb",
    "bz",
    "l",
    "pp",
    "v",
    "pg",
    "fg",
    "sa",
    "sh",
    "ur",
    "sp",
    "op",
    "ec",
  ].map(p => [p, "energy_chem"]),
  ...["a", "b", "m", "y", "p", "oi", "ro", "rm", "rs", "pk", "so"].map(p => [p, "oilseeds"]),
  ...[
    "c",
    "cs",
    "sr",
    "cf",
    "cy",
    "ap",
    "cj",
    "jd",
    "lh",
    "rr",
    "jr",
    "ri",
    "er",
    "lr",
    "wh",
    "ws",
    "wt",
    "pm",
    "bb",
    "fb",
    "lg",
  ].map(p => [p, "agriculture"]),
  ...["if", "ih", "ic", "im", "ts", "tf", "t", "tl"].map(p => [p, "financial"]),
]);

const SECTOR_TITLES: readonly [SectorId, string][] = [
  ["black", "黑色"],
  ["nonferrous", "有色"],
  ["precious", "贵金属"],
  ["energy_chem", "能化"],
  ["oilseeds", "油脂油料"],
  ["agriculture", "农副"],
  ["financial", "金融"],
];

function productSector(symbol: string): SectorId | undefined {
  const parts = symbol.trim().split("/");
  const code = (parts.length >= 2 ? parts[1] : parts[0])?.toLowerCase();
  return code ? PRODUCT_SECTORS[code] : undefined;
}

export function builtInUniverses(storedProducts: readonly string[]): Universe[] {
  const pools: Universe[] = [];
  if (storedProducts.length >= 2) {
    pools.push({ name: t("全部已入库"), symbols: storedProducts.join(", ") });
  }
  const bySector = new Map<SectorId, string[]>();
  for (const product of storedProducts) {
    const sector = productSector(product);
    if (!sector) continue;
    const list = bySector.get(sector);
    if (list) list.push(product);
    else bySector.set(sector, [product]);
  }
  for (const [sector, label] of SECTOR_TITLES) {
    const list = bySector.get(sector);
    if (list?.length) pools.push({ name: t(label), symbols: list.join(", ") });
  }
  return pools;
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
  return (
    symbols.length === evaluation.symbols.length &&
    symbols.every(symbol => evaluation.symbols.includes(symbol)) &&
    sameDay(form.begin, begin, evaluation.firstDay) &&
    sameDay(form.end, end, evaluation.lastDay) &&
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

// The evaluation the factor form describes. A factor takes one value of each
// setting: comparing values is a matter of evaluating again.
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
  };
  return request;
}

// The running sum of the days' IC; a day without one adds nothing.
export function cumulativeIc(days: readonly FactorDay[]) {
  let total = 0;
  return days.map(day => (total += day.ic ?? 0));
}

export function factorTurnover(result: FactorResult | undefined): number | undefined {
  if (!result?.groups.length) return undefined;
  const values = result.groups
    .map(group => group.turnover)
    .filter((value): value is number => value !== undefined && Number.isFinite(value));
  if (!values.length) return undefined;
  return values.reduce((sum, value) => sum + value, 0) / values.length;
}

export type YearlyIc = {
  year: string;
  days: number;
  mean: number;
  deviation?: number;
  ir?: number;
  t?: number;
  positiveShare: number;
};

// Yearly IC breakdown matching factor.py::statistics (sample std ddof=1 and overlap-adjusted t).
export function yearlyIc(days: readonly FactorDay[], horizon: number): YearlyIc[] {
  const byYear = new Map<string, number[]>();
  for (const day of days) {
    if (day.ic === undefined || !Number.isFinite(day.ic)) continue;
    const year = day.tradingDay.slice(0, 4);
    if (!year) continue;
    const bucket = byYear.get(year);
    if (bucket) bucket.push(day.ic);
    else byYear.set(year, [day.ic]);
  }
  const effectiveHorizon = Math.max(1, horizon);
  return [...byYear.entries()]
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([year, values]) => {
      const count = values.length;
      const mean = values.reduce((sum, value) => sum + value, 0) / count;
      const positiveShare = values.filter(value => value > 0).length / count;
      let deviation: number | undefined;
      let ir: number | undefined;
      let tStat: number | undefined;
      if (count > 1) {
        const variance = values.reduce((sum, value) => sum + (value - mean) ** 2, 0) / (count - 1);
        deviation = Math.sqrt(variance);
        if (deviation > 0) {
          ir = mean / deviation;
          tStat = ir * Math.sqrt(count / effectiveHorizon);
        }
      }
      return { year, days: count, mean, deviation, ir, t: tStat, positiveShare };
    });
}

export function strategyClassName(name: string) {
  return name.replace(/(^|_)([a-zA-Z0-9])/g, (_, __, letter: string) => letter.toUpperCase());
}

function pythonLiteral(value: { value: { case?: string; value?: unknown } }): string {
  switch (value.value.case) {
    case "flag":
      return value.value.value ? "True" : "False";
    case "integer":
      return String(value.value.value);
    case "number": {
      const n = Number(value.value.value);
      return Number.isInteger(n) ? `${n}.0` : String(n);
    }
    case "text":
      return JSON.stringify(String(value.value.value));
    default:
      return "None";
  }
}

export function factorStrategyStem(result: FactorResult) {
  const names = (
    result.factorNames.length ? result.factorNames : [result.evaluation?.factor ?? "factor"]
  )
    .map(id => id.split(".")[1] ?? id)
    .map(part => part.replace(/[^A-Za-z0-9_]/g, "_"))
    .filter(Boolean);
  const raw = `${names.slice(0, 2).join("_") || "factor"}_strategy`
    .replace(/^[^A-Za-z_]+/, "")
    .replace(/_+/g, "_");
  return (raw || "factor_strategy").slice(0, 40).replace(/_+$/, "") || "factor_strategy";
}

// Generate a runnable, deployable cross-sectional Strategy from an evaluated factor (or composite).
export function factorStrategyTemplate(result: FactorResult, moduleName: string) {
  const evaluation = result.evaluation;
  const primaryId = evaluation?.factor || result.factorNames[0] || "price.momentum";
  const specs: Array<{
    id: string;
    module: string;
    fn: string;
    params: Record<string, { value: { case?: string; value?: unknown } }>;
  }> = [];
  const pushSpec = (
    id: string,
    params: Record<string, { value: { case?: string; value?: unknown } }> = {},
  ) => {
    const [module, fn] = id.split(".");
    if (!module || !fn) return;
    specs.push({ id, module, fn, params });
  };
  pushSpec(primaryId, evaluation?.params ?? {});
  for (const extra of evaluation?.extraFactors ?? []) {
    pushSpec(extra.factor, extra.params ?? {});
  }
  if (specs.length === 1 && result.factorNames.length >= 2) {
    for (const id of result.factorNames.slice(1)) pushSpec(id, {});
  }

  // Resolve unique local aliases for imported factor functions.
  const usedNames = new Set<string>(["_volatility"]);
  const aliases = specs.map(spec => {
    let alias = spec.fn;
    if (usedNames.has(alias)) alias = `${spec.module}_${spec.fn}`;
    while (usedNames.has(alias)) alias = `${alias}_f`;
    usedNames.add(alias);
    return alias;
  });
  const factorImports = new Map<string, string[]>();
  specs.forEach((spec, idx) => {
    const alias = aliases[idx]!;
    const clause = alias === spec.fn ? spec.fn : `${spec.fn} as ${alias}`;
    const list = factorImports.get(spec.module) ?? [];
    if (!list.includes(clause)) list.push(clause);
    factorImports.set(spec.module, list);
  });

  const composite = specs.length >= 2;
  const winsorize = Boolean(evaluation?.winsorize);
  const standardize = evaluation?.standardize || (composite ? "zscore" : "");
  const sectorNeutral = evaluation?.neutralize === "sector";
  const strategyImports = ["WEIGHT_STEP", "Strategy", "liquid_symbols"];
  if (winsorize) strategyImports.push("winsorize_mad");
  if (standardize === "zscore") strategyImports.push("cross_section_zscore");
  if (standardize === "rank") strategyImports.push("cross_section_rank");
  if (sectorNeutral) strategyImports.push("sector_neutralize");

  let maxWindow = 20;
  for (const spec of specs) {
    for (const val of Object.values(spec.params)) {
      if (val.value.case === "integer") {
        const n = Number(val.value.value);
        if (Number.isFinite(n) && n > maxWindow && n <= 500) maxWindow = n;
      }
    }
  }
  const factorNeed = maxWindow + 2;
  const rebalance = evaluation?.horizons[0] ? Math.max(1, evaluation.horizons[0]) : 5;
  const groups = Math.max(2, evaluation?.groups || 5);
  const symbolCount = result.symbols.length || evaluation?.symbols.length || 6;
  const tailCount = Math.max(1, Math.min(3, Math.floor(symbolCount / groups)));
  const firstMean = result.horizons[0]?.mean;
  const direction = firstMean !== undefined && firstMean < 0 ? -1 : 1;
  const minVolume = evaluation?.minVolume ? evaluation.minVolume : 0.0;
  const minOpenInterest = evaluation?.minOpenInterest ? evaluation.minOpenInterest : 0.0;

  const cls = strategyClassName(moduleName);
  const summary = specs.map(spec => spec.id).join(" + ");
  const importLines = [
    ...[...factorImports.entries()]
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([mod, items]) => `from factors.${mod} import ${items.join(", ")}`),
    "from factors.price import volatility as _volatility",
  ];

  const scoreLines: string[] = [];
  if (composite) {
    scoreLines.push("        components = []");
    specs.forEach((spec, idx) => {
      const alias = aliases[idx]!;
      const kwargs = Object.entries(spec.params)
        .map(([k, v]) => `, ${k}=${pythonLiteral(v)}`)
        .join("");
      scoreLines.push(
        `        part_${idx} = {s: float(ctx.factor(${alias}, s, self.need${kwargs})) for s in eligible}`,
      );
      if (winsorize) scoreLines.push(`        part_${idx} = winsorize_mad(part_${idx})`);
      scoreLines.push(
        `        part_${idx} = ${standardize === "rank" ? "cross_section_rank" : "cross_section_zscore"}(part_${idx})`,
      );
      scoreLines.push(`        components.append(part_${idx})`);
    });
    scoreLines.push("        scores = {}");
    scoreLines.push("        for s in eligible:");
    scoreLines.push("            vals = [c[s] for c in components if s in c]");
    scoreLines.push("            if vals:");
    scoreLines.push("                scores[s] = sum(vals) / len(vals)");
    if (sectorNeutral) scoreLines.push("        scores = sector_neutralize(scores)");
  } else {
    const spec = specs[0]!;
    const alias = aliases[0]!;
    const kwargs = Object.entries(spec.params)
      .map(([k, v]) => `, ${k}=${pythonLiteral(v)}`)
      .join("");
    scoreLines.push(
      `        scores = {s: float(ctx.factor(${alias}, s, self.need${kwargs})) for s in eligible}`,
    );
    scoreLines.push("        scores = {s: v for s, v in scores.items() if np.isfinite(v)}");
    if (winsorize) scoreLines.push("        scores = winsorize_mad(scores)");
    if (standardize === "zscore") scoreLines.push("        scores = cross_section_zscore(scores)");
    if (standardize === "rank") scoreLines.push("        scores = cross_section_rank(scores)");
    if (sectorNeutral) scoreLines.push("        scores = sector_neutralize(scores)");
  }

  const content = `from decimal import Decimal

import numpy as np

from asterion_strategy import ${strategyImports.join(", ")}
${importLines.join("\n")}


class ${cls}(Strategy):
    """Cross-sectional inverse-volatility portfolio generated from ${summary}."""

    params = {
        "rebalance": ${rebalance},
        "count": ${tailCount},
        "gross_weight": 0.6,
        "volatility": 20,
        "direction": ${direction},
        "min_volume": ${Number.isInteger(minVolume) ? `${minVolume}.0` : minVolume},
        "min_open_interest": ${Number.isInteger(minOpenInterest) ? `${minOpenInterest}.0` : minOpenInterest},
    }

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if (
            p["rebalance"] < 1
            or p["count"] < 1
            or p["gross_weight"] <= 0
            or p["volatility"] < 2
            or p["direction"] not in (-1, 1)
            or p["min_volume"] < 0
            or p["min_open_interest"] < 0
        ):
            raise ValueError("invalid factor strategy parameters")
        self.need = max(${factorNeed}, p["volatility"] + 1)
        self._steps = 0

    def ready(self, ctx):
        required = min(len(ctx.symbols), 2 * self.params["count"])
        warm = sum(len(ctx.history(s, "close", self.need)) == self.need for s in ctx.symbols)
        return warm >= required

    def on_bars(self, ctx, bars):
        if not self.ready(ctx):
            return
        self._steps += 1
        p = self.params
        if (self._steps - 1) % p["rebalance"]:
            return
        eligible = liquid_symbols(
            ctx,
            bars,
            min_bars=self.need,
            min_volume=p["min_volume"],
            min_open_interest=p["min_open_interest"],
        )
        risk = {
            s: float(ctx.factor(_volatility, s, p["volatility"] + 1, window=p["volatility"]))
            for s in eligible
        }
        eligible = [s for s in eligible if np.isfinite(risk[s]) and risk[s] > 0]
${scoreLines.join("\n")}
        scores = {s: p["direction"] * v for s, v in scores.items() if np.isfinite(v)}
        ordered = sorted(scores, key=scores.get, reverse=True)
        count = p["count"]
        target = dict.fromkeys(ctx.symbols, 0.0)
        if len(ordered) < 2 * count or scores[ordered[count - 1]] <= scores[ordered[-count]]:
            ctx.weights(target)
            return
        budget = int(Decimal(str(p["gross_weight"])) / (2 * WEIGHT_STEP))
        for side_symbols, side in ((ordered[:count], 1), (ordered[-count:], -1)):
            total = sum(1.0 / risk[s] for s in side_symbols)
            remaining = budget
            for i, s in enumerate(side_symbols):
                units = remaining if i == len(side_symbols) - 1 else int(budget / risk[s] / total)
                target[s] = side * float(units * WEIGHT_STEP)
                remaining -= units
        ctx.weights(target)
`;
  return { content, id: `${moduleName}.${cls}` };
}

export function copyStem(symbol: string | undefined, path: string) {
  if (!symbol) return `${path.replace(/^(strategies|factors)\//, "").replace(/\.py$/, "")}_copy`;
  const snake = symbol
    .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
    .replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2")
    .toLowerCase();
  return `${snake}_copy`;
}

export function copiedModule(
  folder: "strategies" | "factors",
  content: string,
  symbol: string | undefined,
  name: string,
) {
  if (!symbol) return { content };
  const target = folder === "strategies" ? strategyClassName(name) : name;
  const pattern =
    folder === "strategies"
      ? /^class\s+([A-Za-z_][A-Za-z0-9_]*)\b/gm
      : /^def\s+([A-Za-z_][A-Za-z0-9_]*)\b/gm;
  const siblings = [...content.matchAll(pattern)]
    .map(match => match[1]!)
    .filter(item => item !== symbol && !item.startsWith("_"));
  let next = content;
  for (const other of siblings) {
    next = next.replace(new RegExp(`\\b${other}\\b`, "g"), `_${other}`);
  }
  if (target !== symbol) {
    next = next.replace(new RegExp(`\\b${symbol}\\b`, "g"), target);
  }
  return { content: next, id: `${name}.${target}` };
}

// The first content of a new module of strategies/ or factors/, which runs as it is.
export function moduleTemplate(folder: "strategies" | "factors", name: string) {
  if (folder === "factors")
    return `import numpy as np


def ${name}(close, window: int = 20):
    """Return over the last \`window\` bars."""
    result = np.full(len(close), np.nan)
    result[window:] = close[window:] / close[:-window] - 1
    return result
`;
  const cls = strategyClassName(name);
  return `from asterion_strategy import Strategy

from factors.price import momentum


class ${cls}(Strategy):
    """Long while the return over \`lookback\` bars is positive, short while it is negative."""
    params = {"lookback": 20, "lots": 1}

    def ready(self, ctx):
        need = self.params["lookback"] + 1
        return all(len(ctx.history(s, "close", need)) == need for s in ctx.symbols)

    def on_bar(self, ctx, bar):
        lookback = self.params["lookback"]
        value = ctx.factor(momentum, bar.symbol, lookback + 1, lookback=lookback)
        if value > 0:
            ctx.target(bar.symbol, self.params["lots"])
        elif value < 0:
            ctx.target(bar.symbol, -self.params["lots"])
`;
}
