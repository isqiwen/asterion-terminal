import { describe, expect, it } from "vitest";
import { parameterError, type ParameterDefinition } from "./ParameterFields";
const fields: ParameterDefinition[] = [
  { key: "ratio", label: "阈值", type: "decimal", minimum: "0.1000", maximum: "0.1001", scale: 4, default: "0.1000" },
  { key: "enabled", label: "开关", type: "boolean", default: true },
  { key: "mode", label: "模式", type: "enum", choices: [{ value: "a", label: "甲" }], default: "a" },
];
const values = { ratio: "0.1000", enabled: false, mode: "a" };
describe("declared parameter validation", () => {
  it("preserves decimal text and false with exact boundary checks", () => {
    expect(parameterError(fields, values)).toBe("");
    expect(parameterError(fields, { ...values, ratio: "0.1001" })).toBe("");
    expect(values.ratio).toBe("0.1000");
  });
  it.each([0.1, "1e-1", "0.10001", "0.1002", "NaN", null])("rejects invalid decimal %s", (ratio) => {
    expect(parameterError(fields, { ...values, ratio })).not.toBe("");
  });
  it("rejects missing, unknown, coerced and unavailable values", () => {
    expect(parameterError(fields, { ...values, enabled: "false" })).not.toBe("");
    expect(parameterError(fields, { ...values, mode: "b" })).not.toBe("");
    expect(parameterError(fields, {})).not.toBe("");
    expect(parameterError(fields, { ...values, extra: 1 })).not.toBe("");
  });
});
