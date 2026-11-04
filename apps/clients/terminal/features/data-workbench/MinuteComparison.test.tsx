import { act, fireEvent, render, screen, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { beforeEach, expect, test, vi } from "vitest";
import {
  MinuteComparisonSchema,
  MinuteDifferencesSchema,
} from "../../src/generated/asterion/market_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { MinuteComparison } from "./MinuteComparison";
import { feature } from "./feature";

const request = vi.hoisted(() => vi.fn());
vi.mock("../../src/bridge/market-data", () => ({ marketData: request }));
registerTerminalFeatures([feature]);
beforeEach(() => {
  request.mockReset();
});

const values = (close: string) => ({
  open: { text: "3100" },
  high: { text: "3110" },
  low: { text: "3090" },
  close: { text: close },
  volume: 12n,
});
const open = 1793322000000; // 2026-10-30 09:00 Beijing

test("nothing is shown while Tushare and the recorder agree", async () => {
  request.mockResolvedValue(create(MinuteComparisonSchema, { days: 12, differingDays: 0 }));
  const view = render(<MinuteComparison />);
  await act(async () => {});
  expect(request).toHaveBeenCalledWith({ case: "compareMinutes", value: {} });
  expect(view.container.innerHTML).toBe("");
});

test("a disagreeing contract-day opens its minutes with both sources' values", async () => {
  request.mockImplementation(async (operation: { case: string }) =>
    operation.case === "compareMinutes"
      ? create(MinuteComparisonSchema, {
          days: 12,
          differingDays: 1,
          differing: [
            {
              contract: "SHFE/rb/2027-01",
              tradingDay: "2026-10-30",
              compared: 224,
              differing: 1,
              tushareOnly: 1,
            },
          ],
        })
      : create(MinuteDifferencesSchema, {
          minutes: [
            { startMs: BigInt(open), tushare: values("3105"), recorder: values("3106") },
            { startMs: BigInt(open + 60000), tushare: values("3101") },
          ],
        }),
  );
  render(<MinuteComparison />);
  expect(
    await screen.findByText("两个来源都有的合约交易日 12 个，其中 1 个不一致。"),
  ).toBeDefined();
  const days = screen.getByRole("table", { name: "分钟线不一致的合约交易日" });
  fireEvent.click(within(days).getAllByRole("row")[1]!);
  const minutes = await screen.findByRole("table", {
    name: "SHFE/rb/2027-01 2026-10-30 不一致的分钟",
  });
  expect(request).toHaveBeenCalledWith({
    case: "getMinuteDifferences",
    value: { contract: "SHFE/rb/2027-01", tradingDay: "2026-10-30" },
  });
  const [differing, missing] = within(minutes).getAllByRole("row").slice(2);
  const texts = (row: HTMLElement) =>
    within(row)
      .getAllByRole("cell")
      .map(cell => cell.textContent);
  expect(texts(differing!)).toEqual([
    "09:00",
    "3100",
    "3110",
    "3090",
    "3105",
    "12",
    "3100",
    "3110",
    "3090",
    "3106",
    "12",
  ]);
  // Only the close disagrees; a minute one source lacks differs throughout.
  const marked = (row: HTMLElement) =>
    within(row)
      .getAllByRole("cell")
      .map(cell => cell.hasAttribute("data-differs"));
  expect(marked(differing!)).toEqual([
    false,
    false,
    false,
    false,
    true,
    false,
    false,
    false,
    false,
    true,
    false,
  ]);
  expect(texts(missing!).slice(6)).toEqual(["—", "—", "—", "—", "—"]);
  expect(marked(missing!).slice(1).every(Boolean)).toBe(true);
});
