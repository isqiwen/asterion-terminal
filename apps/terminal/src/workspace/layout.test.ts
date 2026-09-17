import { expect, test } from "vitest";
import { parseLayout, views } from "./layout";
test("rejects old layouts and validates current workspace bounds", () => {
  expect(views).not.toContain("机器与服务");
  expect(parseLayout("{")).toMatchObject({
    version: 4,
    view: "市场",
    inspector: false,
    tasks: false,
    taskHeight: 180,
  });
  expect(
    parseLayout('{"version":1,"view":"机器与服务","inspector":false}'),
  ).toMatchObject({ version: 4, view: "市场", inspector: false });
  expect(
    parseLayout('{"version":4,"view":"数据","inspector":false}').view,
  ).toBe("数据");
  expect(
    parseLayout('{"version":4,"view":"研究","taskHeight":900}').taskHeight,
  ).toBe(400);
});
