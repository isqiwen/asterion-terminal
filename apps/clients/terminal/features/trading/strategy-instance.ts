import { create } from "@bufbuild/protobuf";
import { Period, Side } from "../../src/generated/asterion/common_pb";
import {
  InstanceState,
  type InstanceStatus,
  type Position,
} from "../../src/generated/asterion/trading_pb";
import {
  ParameterValueSchema,
  type ParameterValue,
} from "../../src/generated/asterion/strategy_pb";
import { decimalText } from "../../src/bridge/decimal";
import { key, t } from "./live-shared";

export type ParameterInput = {
  name: string;
  kind: "integer" | "number" | "flag" | "text";
  value: string;
};

const maximum = (1n << 63n) - 1n;

// The bar periods a live strategy can run on.
export const strategyPeriods = [
  Period.MINUTES_1,
  Period.MINUTES_5,
  Period.MINUTES_10,
  Period.MINUTES_15,
  Period.MINUTES_30,
  Period.MINUTES_60,
  Period.DAY,
];
export const periodText = (period: Period) =>
  period === Period.DAY
    ? t("日线")
    : t("{count} 分钟", { count: Period[period].replace("MINUTES_", "") });

export function parameterValues(rows: ParameterInput[]): Record<string, ParameterValue> {
  const entries: [string, ParameterValue][] = [];
  for (const row of rows) {
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
    entries.push([row.name, create(ParameterValueSchema, { value })]);
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

// Long minus short lots of a contract, today's and earlier ones together.
function held(positions: readonly Position[], contract: string) {
  let lots = 0;
  for (const position of positions)
    if (key(position.instrument) === contract)
      lots +=
        (position.side === Side.BUY ? 1 : -1) *
        (Number(decimalText(position.todayLots)) + Number(decimalText(position.yesterdayLots)));
  return lots;
}

// Each contract an instance trades with its latest target and the account's net
// lots. A dominant series lists the months it targets or holds, else its first.
export function targetRows(instance: InstanceStatus, positions?: readonly Position[]) {
  const configuration = instance.instance;
  const targetOf = (name: string) => {
    const size = instance.lastDecision?.targets.find(item => key(item.instrument) === name)?.size;
    return size?.case === "lots"
      ? size.value.toString()
      : size?.case === "weight"
        ? `${decimalText(size.value)} ${t("权重")}`
        : "—";
  };
  const row = (label: string, name: string) => ({
    name,
    label,
    target: targetOf(name),
    net: positions ? String(held(positions, name)) : "—",
  });
  return [
    ...(configuration?.instruments ?? []).map(instrument => row(key(instrument), key(instrument))),
    ...(configuration?.series ?? []).flatMap(series => {
      const active = series.contracts.filter(entry => {
        const name = key(entry.instrument);
        return (
          instance.lastDecision?.targets.some(item => key(item.instrument) === name) ||
          (positions && held(positions, name) !== 0)
        );
      });
      const shown = active.length ? active : series.contracts.slice(0, 1);
      return shown.map(entry =>
        row(
          `${series.exchange}.${series.product} · ${key(entry.instrument)}`,
          key(entry.instrument),
        ),
      );
    }),
  ];
}
