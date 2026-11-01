import { expect, test } from "vitest";
import { parameterValues, weightText, weightUnits } from "./strategy-instance";

test("budgets preserve eight decimal places and explicit zero without rounding", () => {
  expect(weightUnits("0")).toBe(0n);
  expect(weightUnits("1.00000001")).toBe(100000001n);
  expect(weightUnits("92233720368.54775807")).toBe(9223372036854775807n);
  expect(weightText(-100000001n)).toBe("-1.00000001");
  for (const invalid of ["", "-1", "0.000000001", "1e2", "92233720368.54775808"])
    expect(() => weightUnits(invalid)).toThrow();
});

test("typed parameters retain int64, false and empty text and reject ambiguous values", () => {
  const values = parameterValues([
    { id: 1, name: "count", kind: "integer", value: "9223372036854775807" },
    { id: 2, name: "enabled", kind: "flag", value: "false" },
    { id: 3, name: "label", kind: "text", value: "" },
  ]);
  expect(values.count.value).toEqual({ case: "integer", value: 9223372036854775807n });
  expect(values.enabled.value).toEqual({ case: "flag", value: false });
  expect(values.label.value).toEqual({ case: "text", value: "" });
  expect(() =>
    parameterValues([{ id: 1, name: "count", kind: "integer", value: "9223372036854775808" }]),
  ).toThrow();
  expect(() =>
    parameterValues([{ id: 1, name: "ratio", kind: "number", value: "Infinity" }]),
  ).toThrow();
  expect(() =>
    parameterValues([
      { id: 1, name: "x", kind: "text", value: "a" },
      { id: 2, name: "x", kind: "text", value: "b" },
    ]),
  ).toThrow();
});
