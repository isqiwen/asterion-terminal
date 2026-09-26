import { expect, test } from "vitest";
import { parseLayout } from "./layout";
test("current layout contract rejects invalid input", () => {
  const initial = parseLayout(null);
  expect(initial).toMatchObject({ version: 4, view: "总览" });
  for (const raw of [
    "{",
    JSON.stringify({ ...initial, version: "unknown-schema" }),
    JSON.stringify({ ...initial, dock: undefined }),
    JSON.stringify({ ...initial, taskHeight: 900 }),
  ])
    expect(() => parseLayout(raw)).toThrow("原始记录已保留");
  expect(parseLayout(JSON.stringify({ ...initial, view: "数据" })).view).toBe(
    "数据",
  );
});
