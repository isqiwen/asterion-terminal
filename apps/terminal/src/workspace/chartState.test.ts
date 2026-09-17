import { expect, test } from "vitest";
import { parseLayout } from "./layout";
import { parseViewport } from "./chartState";
test("old layouts default safely and malformed chart ranges cannot reach the chart", () => {
  expect(parseLayout('{"version":3,"view":"市场"}')).toMatchObject({
    viewport: null,
    dock: "left",
  });
  expect(
    parseViewport({ snapshot: "s", contract: "c", from: 20, to: 10 }),
  ).toBeNull();
  expect(
    parseViewport({ snapshot: "s", contract: "c", from: NaN, to: 10 }),
  ).toBeNull();
  const viewport = { snapshot: "s", contract: "c", from: -10, to: 30 };
  expect(
    parseLayout(JSON.stringify({ version: 4, dock: "bottom", viewport })),
  ).toMatchObject({ dock: "bottom", viewport });
});
