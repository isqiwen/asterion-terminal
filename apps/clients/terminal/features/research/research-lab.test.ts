import { create } from "@bufbuild/protobuf";
import { expect, test } from "vitest";
import { RunDaySchema, StrategyParameterSchema } from "../../src/generated/asterion/lab_pb";
import { setLocale } from "../../src/i18n";
import "./feature";
import {
  defaultForm,
  heatmap,
  monthlyReturns,
  parameterValues,
  productsOf,
  runRequest,
} from "./research-lab";

setLocale("zh-CN");
const integer = (name: string, value: bigint) =>
  create(StrategyParameterSchema, {
    name,
    kind: "int",
    default: { value: { case: "integer", value } },
  });

test("a field's values follow the parameter's kind, and several make a sweep", () => {
  expect(parameterValues(integer("fast", 5n), "3, 5，8").map(v => v.value)).toEqual([
    { case: "integer", value: 3n },
    { case: "integer", value: 5n },
    { case: "integer", value: 8n },
  ]);
  expect(() => parameterValues(integer("fast", 5n), "3.5")).toThrow("不是整数");
  expect(() => parameterValues(integer("fast", 5n), "")).toThrow("没有值");
  const form = { ...defaultForm(), symbols: "SHFE/rb SHFE/au/2026-12", params: { fast: "3,5" } };
  const { run, swept } = runRequest(
    form,
    "/repo",
    "ma_cross.MaCross",
    [integer("fast", 5n), integer("slow", 20n)],
    "x",
  );
  expect(swept.map(item => item.name)).toEqual(["fast"]);
  expect(run.params).toEqual({ slow: { value: { case: "integer", value: 20n } } });
  expect(run.costs?.map(item => item.product)).toEqual(["SHFE/rb", "SHFE/au"]);
  expect(productsOf("SHFE/rb, SHFE/rb/2026-05")).toEqual(["SHFE/rb"]);
});

test("monthly returns compound from the deposit, and a sweep grid fills by its two parameters", () => {
  const days = [
    ["2026-01-05", "1010000"],
    ["2026-01-30", "1020000"],
    ["2026-02-02", "1000000"],
    ["2026-02-27", "1050000"],
  ].map(([tradingDay, equity]) => create(RunDaySchema, { tradingDay, equity }));
  const months = monthlyReturns("1000000", days);
  expect(months.map(m => m.month)).toEqual(["2026-01", "2026-02"]);
  expect(months[0].value).toBeCloseTo(0.02);
  expect(months[1].value).toBeCloseTo(1050000 / 1020000 - 1);
  const grid = heatmap(
    [
      { params: { fast: "10", slow: "20" }, metric: 1 },
      { params: { fast: "5", slow: "20" }, metric: 2 },
      { params: { fast: "5", slow: "30" }, metric: null },
    ],
    "fast",
    "slow",
  );
  expect(grid).toEqual({
    rows: ["5", "10"],
    columns: ["20", "30"],
    cells: [
      [2, null],
      [1, null],
    ],
  });
});
