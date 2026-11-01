import { create } from "@bufbuild/protobuf";
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

export function weightUnits(text: string): bigint {
  if (!/^\d+(\.\d{1,8})?$/.test(text)) throw new Error(t("权重预算必须是非负数，最多八位小数。"));
  const [whole, fraction = ""] = text.split(".");
  const value = BigInt(whole) * 100000000n + BigInt(fraction.padEnd(8, "0"));
  if (value > maximum) throw new Error(t("权重预算超出定点数范围。"));
  return value;
}

export function weightText(value: bigint): string {
  const absolute = value < 0n ? -value : value;
  const fraction = (absolute % 100000000n).toString().padStart(8, "0").replace(/0+$/, "");
  return `${value < 0n ? "-" : ""}${absolute / 100000000n}${fraction ? `.${fraction}` : ""}`;
}

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

export const activeStrategy = (state?: string) =>
  state === "starting" || state === "warming" || state === "ready";
