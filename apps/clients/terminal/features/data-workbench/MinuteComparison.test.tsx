import { render, screen, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { expect, test, vi } from "vitest";
import { ComparisonSchema } from "../../src/generated/asterion/dataset_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { MinuteComparison } from "./MinuteComparison";
import { feature } from "./feature";

const request = vi.hoisted(() => vi.fn());
vi.mock("../../src/bridge/market-data", () => ({ marketData: request }));
registerTerminalFeatures([feature]);

test("the contract-days where Tushare and the recorder disagree are listed with their minutes", async () => {
  request.mockResolvedValue(
    create(ComparisonSchema, {
      days: 12,
      differingDays: 1,
      differing: [
        {
          contract: "SHFE/rb/2027-01",
          day: "2026-10-30",
          compared: 224,
          differing: 3,
          tushareOnly: 1,
          recorderOnly: 0,
          partial: 2,
        },
      ],
    }),
  );
  render(<MinuteComparison />);
  expect(
    await screen.findByText("两个来源都有的合约交易日 12 个，其中 1 个不一致。"),
  ).toBeDefined();
  expect(request).toHaveBeenCalledWith({ case: "minuteComparison", value: {} }, "minuteComparison");
  const row = within(screen.getByRole("table", { name: "分钟线不一致的合约交易日" })).getAllByRole(
    "row",
  )[1]!;
  expect(
    within(row)
      .getAllByRole("cell")
      .map(cell => cell.textContent),
  ).toEqual(["SHFE/rb/2027-01", "2026-10-30", "224", "3", "1", "0", "2"]);
});
