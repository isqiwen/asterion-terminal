import { create } from "@bufbuild/protobuf";
import { Period } from "../../src/generated/asterion/common_pb";
import { InstanceState } from "../../src/generated/asterion/trading_pb";
import {
  ParameterValueSchema,
  type ParameterValue,
} from "../../src/generated/asterion/strategy_pb";
import { t } from "./live-shared";

export type ParameterInput = {
  id: number;
  name: string;
  kind: "integer" | "number" | "flag" | "text";
  value: string;
};

const maximum = (1n << 63n) - 1n;

// The bar periods a live strategy can run on.
export const strategyPeriods: [Period, string][] = [
  [Period.MINUTES_1, "1m"],
  [Period.MINUTES_5, "5m"],
  [Period.MINUTES_10, "10m"],
  [Period.MINUTES_15, "15m"],
  [Period.MINUTES_30, "30m"],
  [Period.MINUTES_60, "60m"],
  [Period.DAY, "1d"],
];
export const periodText = (period: Period) =>
  strategyPeriods.find(([value]) => value === period)?.[1] ?? Period[period];

export function parameterValues(rows: ParameterInput[]): Record<string, ParameterValue> {
  const entries: [string, ParameterValue][] = [];
  const names = new Set<string>();
  for (const row of rows) {
    const name = row.name.trim();
    if (!name || names.has(name)) throw new Error(t("参数名不能为空或重复。"));
    names.add(name);
    let value: ParameterValue["value"];
    if (row.kind === "integer") {
      if (!/^-?\d+$/.test(row.value)) throw new Error(t("整数参数必须是完整的十进制整数。"));
      const integer = BigInt(row.value);
      if (integer < -maximum - 1n || integer > maximum) throw new Error(t("整数参数超出范围。"));
      value = { case: "integer", value: integer };
    } else if (row.kind === "number") {
      const number = Number(row.value);
      if (!row.value.trim() || !Number.isFinite(number))
        throw new Error(t("小数参数必须是有限数值。"));
      value = { case: "number", value: number };
    } else if (row.kind === "flag") {
      value = { case: "flag", value: row.value === "true" };
    } else {
      value = { case: "text", value: row.value };
    }
    entries.push([name, create(ParameterValueSchema, { value })]);
  }
  return Object.fromEntries(entries);
}

export function parameterText(parameter: ParameterValue): string {
  return parameter.value.case === "flag"
    ? t(parameter.value.value ? "是" : "否")
    : String(parameter.value.value ?? "—");
}

export const activeStrategy = (state?: InstanceState) =>
  state === InstanceState.STARTING ||
  state === InstanceState.WARMING ||
  state === InstanceState.READY;
