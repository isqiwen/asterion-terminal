import { expect, test } from "vitest";
import { parameterValues } from "./strategy-instance";

test("typed parameters retain int64, false and empty text and reject ambiguous values", () => {
  const values = parameterValues([
    { name: "count", kind: "integer", value: "9223372036854775807" },
    { name: "enabled", kind: "flag", value: "false" },
    { name: "label", kind: "text", value: "" },
  ]);
  expect(values.count.value).toEqual({ case: "integer", value: 9223372036854775807n });
  expect(values.enabled.value).toEqual({ case: "flag", value: false });
  expect(values.label.value).toEqual({ case: "text", value: "" });
  expect(() =>
    parameterValues([{ name: "count", kind: "integer", value: "9223372036854775808" }]),
  ).toThrow();
  expect(() => parameterValues([{ name: "ratio", kind: "number", value: "Infinity" }])).toThrow();
});
