import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { FactorMembership } from "./FactorMembership";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const table = (instrument: string) => ({
  kind: "table" as const,
  data: {
    instrument,
    snapshot: "a".repeat(64),
    daily_source: "fixture.daily",
    calendar_source: "fixture.calendar",
    begin_day: "2026-01-01",
    end_day: "2026-01-02",
    acquired_at_ns: "1",
    months: [],
    rolls: [],
    terms: 0,
    count: 2,
  },
});
test("daily member evidence distinguishes qualification, missing facts and evaluation tail", () => {
  render(
    <FactorMembership
      experiment={{
        liquidity: {
          minimum_history: 3,
          minimum_volume: "9007199254740993",
          minimum_open_interest: "0",
        },
        series: [table("SHFE/rb"), table("SHFE/cu"), table("SHFE/al")],
      }}
      result={{
        memberships: [
          { series_index: 0, trading_day: "2026-01-01", reason: "MEMBERSHIP_ELIGIBLE" },
          { series_index: 1, trading_day: "2026-01-01", reason: "MEMBERSHIP_UNKNOWN_VOLUME" },
          { series_index: 0, trading_day: "2026-01-02", reason: "MEMBERSHIP_ELIGIBLE" },
          {
            series_index: 2,
            trading_day: "2026-01-02",
            reason: "MEMBERSHIP_UNKNOWN_OPEN_INTEREST",
          },
        ],
        cross_sections: [
          {
            event_index: 0,
            observed: "2026-01-01",
            label: "2026-01-02",
            pearson: null,
            spearman: null,
            series: 1,
          },
        ],
      }}
    />,
  );
  fireEvent.click(screen.getByText("每日资格与排除原因"));
  const members = screen.getByRole("table", { name: "每日成员资格" });
  expect(
    within(members)
      .getAllByRole("cell")
      .map(cell => cell.textContent),
  ).toEqual(["SHFE/rb", "有资格", "SHFE/cu", "上一观测成交量未知", "SHFE/al", "未观测"]);
  expect(screen.getByText(/固定筛选规则/).textContent).toContain("9007199254740993");
  fireEvent.click(screen.getByRole("button", { name: "下一交易日" }));
  expect(
    within(members)
      .getAllByRole("cell")
      .map(cell => cell.textContent),
  ).toEqual(["SHFE/rb", "有资格", "SHFE/cu", "未观测", "SHFE/al", "上一观测持仓量未知"]);
  expect(screen.getByText(/实际评价成员数/).textContent).toContain("—");
});
