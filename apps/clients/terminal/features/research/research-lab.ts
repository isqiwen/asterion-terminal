// What the research page computes without the lab: parameter values from the
// form, the request for a run, a sweep or a factor's evaluation, and figures
// read from a run record.
import type { MessageInitShape } from "@bufbuild/protobuf";
import { type MessageValues, translate } from "../../src/i18n";
import { protocolPeriod } from "../../src/ui/periods";
import { RunState } from "../../src/generated/asterion/lab_pb";
import type {
  BacktestSchema,
  EvaluateFactorSchema,
  FactorDay,
  FactorEntry,
  Parameter,
  RunDay,
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
export function parameterKind(parameter: Parameter) {
  return ({ flag: "bool", integer: "int", number: "float", text: "str" } as Record<string, string>)[
    parameter.default?.value.case ?? "text"
  ]!;
}

export type RunForm = {
  symbols: string;
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
  annualPeriods: string;
  zeroRiskFree: boolean;
  validation: "none" | "holdout" | "rolling";
  holdoutDay: string;
  trainingDays: string;
  validationDays: string;
  processes: string;
};

export function defaultForm(): RunForm {
  return {
    symbols: "",
    begin: "",
    end: "",
    minutes: "0",
    params: {},
    deposit: "1000000",
    slippage: "1",
    maxOrderLots: "",
    maxContractLots: "",
    maxGrossLots: "",
    maxWorkingOrders: "10",
    maxGrossWeight: "1",
    maxNetWeight: "1",
    annualPeriods: "252",
    zeroRiskFree: true,
    validation: "none",
    holdoutDay: "",
    trainingDays: "60",
    validationDays: "20",
    processes: "",
  };
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
    months.push({ month, value: closing / opening - 1 });
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
};

export function defaultFactorForm(): FactorForm {
  return {
    symbols: "",
    begin: "",
    end: "",
    params: {},
    horizons: "1, 2, 3, 5, 10, 20",
    groups: "5",
  };
}

// The evaluation the factor form describes. A factor takes one value of each
// setting: comparing values is a matter of evaluating again.
// Runs, sweeps and factor results are named by their module and the time they
// were asked for, to the millisecond so that two asked for in one second differ.
export function identity(code: string) {
  const stamp = new Date()
    .toISOString()
    .replace(/[-:T.]/g, "")
    .slice(0, 17);
  return `${code.split(".")[0].replace(/[^A-Za-z0-9_-]/g, "-")}-${stamp}`;
}

// An id as shown: its module and that time on the viewer's clock, to the second.
export function recordName(id: string) {
  const match = /^(.*)-(\d{4})(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)\d{3}(.*)$/.exec(id);
  if (!match) return id;
  const [, code, ...parts] = match;
  const [year, month, day, hour, minute, second] = parts.slice(0, 6).map(Number);
  const time = new Date(Date.UTC(year, month - 1, day, hour, minute, second));
  const two = (value: number) => String(value).padStart(2, "0");
  return `${code} ${two(time.getMonth() + 1)}-${two(time.getDate())} ${two(time.getHours())}:${two(time.getMinutes())}:${two(time.getSeconds())}${parts[6]}`;
}

export function factorRequest(form: FactorForm, repository: string, factor: FactorEntry) {
  const symbols = symbolsOf(form.symbols);
  if (!symbols.length) throw new Error(t("请填写品种"));
  const params: Record<string, ValueInit> = {};
  for (const parameter of factor.parameters) {
    const values = parameterValues(
      parameter,
      form.params[parameter.name] ?? defaultText(parameter),
    );
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
  const request: FactorInit = {
    repository,
    factor: factor.id,
    params,
    symbols,
    firstDay: form.begin,
    lastDay: form.end,
    horizons,
    groups,
  };
  return request;
}

// The running sum of the days' IC; a day without one adds nothing.
export function cumulativeIc(days: readonly FactorDay[]) {
  let total = 0;
  return days.map(day => (total += day.ic ?? 0));
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
  const cls = name.replace(/(^|_)([a-z0-9])/g, (_, __, letter: string) => letter.toUpperCase());
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
