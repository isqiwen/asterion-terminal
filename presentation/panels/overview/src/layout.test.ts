import { expect, test } from "vitest";
import { parseDashboardLayout } from "./layout";
import { dashboardWidgets } from "./public";
test("dashboard layout rejects malformed records and preserves unavailable references", () => {
  const value = { version: 1, compact: true, pulse: true, refreshSeconds: 0, items: [{ id: "missing.widget", width: 2 }] };
  expect(parseDashboardLayout(JSON.stringify(value))).toEqual(value);
  for (const change of [{ version: 9 }, { items: [...value.items, ...value.items] }, { items: [{ id: "x", width: 3 }] }, { pulse: null }])
    expect(() => parseDashboardLayout(JSON.stringify({ ...value, ...change }))).toThrow();
});


test("layout column overrides are explicit and validated", () => {
  const value = { version: 1, compact: false, pulse: false, refreshSeconds: 0, items: [{ id: "test", width: 2, column: "secondary" }] };
  expect(parseDashboardLayout(JSON.stringify(value))).toEqual(value);
  expect(() => parseDashboardLayout(JSON.stringify({ ...value, items: [{ ...value.items[0], column: "invalid" }] }))).toThrow();
});
