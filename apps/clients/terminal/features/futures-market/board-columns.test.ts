import { expect, test } from "vitest";
import { boardColumns } from "./board-columns";

// Rows of Shanghai, Zhengzhou, Dalian, financial and Guangzhou.
const groups = [30, 32, 27, 11, 8];

test("groups fill the columns that fit, in order, without an empty or lopsided column", () => {
  expect(boardColumns(groups, 5)).toEqual([[0], [1], [2], [3, 4]]);
  expect(boardColumns(groups, 2)).toEqual([
    [0, 1],
    [2, 3, 4],
  ]);
  expect(boardColumns(groups, 1)).toEqual([[0, 1, 2, 3, 4]]);
});
