// What the research page computes without the lab: parameter values from the
// form, the request for a run or a sweep, and figures read from a run record.
import type { MessageInitShape } from "@bufbuild/protobuf";
import { translate, type MessageValues } from "../contract";
import type {
  BacktestRunSchema,
  ParameterValueSchema,
  RunDay,
  StrategyParameter,
} from "../../src/generated/asterion/lab_pb";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
type ValueInit = MessageInitShape<typeof ParameterValueSchema>;
export type RunInit = MessageInitShape<typeof BacktestRunSchema>;

// The eight cost values of a product, in the lab's order.
export const costNames = [
  "每手保证金",
  "开仓手续费",
  "平今手续费",
  "平昨手续费",
  "保证金率",
  "开仓手续费率",
  "平今手续费率",
  "平昨手续费率",
] as const;

export type RunForm = {
  symbols: string;
  begin: string;
  end: string;
  params: Record<string, string>;
  deposit: string;
  costs: Record<string, string[]>;
  slippage: string;
  maxOrderLots: string;
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
    params: {},
    deposit: "1000000",
    costs: {},
    slippage: "1",
    maxOrderLots: "",
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

// A run's costs are given per product: EXCHANGE/product of every symbol.
export function productsOf(text: string) {
  return [...new Set(symbolsOf(text).map(symbol => symbol.split("/").slice(0, 2).join("/")))];
}

// One field's values: comma-separated, each of the parameter's kind. More
// than one makes the run a sweep.
export function parameterValues(parameter: StrategyParameter, text: string): ValueInit[] {
  const items = text
    .split(/[,，]/)
    .map(item => item.trim())
    .filter(Boolean);
  const name = parameter.name;
  if (!items.length) throw new Error(t("参数 {name} 没有值", { name }));
  return items.map(item => {
    switch (parameter.kind) {
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

export function defaultText(parameter: StrategyParameter) {
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
  parameters: readonly StrategyParameter[],
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
    provider: "tushare",
    symbols,
    beginDay: form.begin,
    endDay: form.end,
    deposit: form.deposit,
    costs: productsOf(form.symbols).map(product => ({
      product,
      values: form.costs[product] ?? costNames.map(() => ""),
    })),
    slippageTicks: Number(form.slippage),
    maxOrderLots: form.maxOrderLots,
    maxGrossLots: form.maxGrossLots,
    maxWorkingOrders: Number(form.maxWorkingOrders),
    maxGrossWeight: form.maxGrossWeight,
    maxNetWeight: form.maxNetWeight,
    annualPeriods: Number(form.annualPeriods) || 0,
    zeroRiskFree: form.zeroRiskFree,
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
    const closing = Number(days[i].equity);
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
