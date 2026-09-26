import { expect, test } from "vitest";
import { parseLayout } from "./layout";
import { parseViewport } from "@asterion/ui-market-panel/chartState";
test("invalid chart ranges are rejected by the current layout contract", () => {
  expect(() =>
    parseLayout(
      JSON.stringify({
        ...parseLayout(null),
        viewport: { snapshot: "s", contract: "c", from: 20, to: 10 },
      }),
    ),
  ).toThrow("工作区布局不符合当前规范");
  expect(
    parseViewport({ snapshot: "s", contract: "c", from: 20, to: 10 }),
  ).toBeNull();
  expect(
    parseViewport({ snapshot: "s", contract: "c", from: NaN, to: 10 }),
  ).toBeNull();
  const viewport = { snapshot: "s", contract: "c", from: -10, to: 30 };
  expect(
    parseLayout(
      JSON.stringify({ ...parseLayout(null), dock: "bottom", viewport }),
    ),
  ).toMatchObject({ dock: "bottom", viewport });
});
