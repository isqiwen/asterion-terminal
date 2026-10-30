import { render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { TradeStatistics } from "./TradeStatistics";
import { feature } from "./feature";
registerTerminalFeatures([feature]);

test("net statistics show actual fees separately from fees on positions still open", () => {
  render(
    <TradeStatistics
      trades={{
        by_contract: [
          { venue: "SHFE", symbol: "rb2610", count: 1, gross: "10", fees: "12", net: "-2" },
        ],
        count: 1,
        winning: 0,
        losing: 1,
        average_win: null,
        average_loss: -2,
        payoff: null,
        gross: "10",
        fees: "12",
        net: "-2",
        open_fees: "3.00000001",
        closed: [{ venue: "SHFE", symbol: "rb2610", gross: "10", fees: "12", net: "-2" }],
      }}
    />,
  );
  const figures = screen.getByRole("region", { name: "按笔统计" });
  expect(within(figures).getByText("净盈利笔数占比").parentElement!.textContent).toContain("0.00%");
  expect(within(figures).getByText("平均净亏损").parentElement!.textContent).toContain("-2.00");
  expect(within(figures).getByText("未闭合持仓手续费").parentElement!.textContent).toContain(
    "3.00000001",
  );
  const contribution = within(figures).getByRole("table", {
    name: "按合约已平仓贡献",
    hidden: true,
  });
  expect(
    within(contribution)
      .getAllByRole("cell", { hidden: true })
      .map(cell => cell.textContent),
  ).toEqual(["SHFE · rb2610", "1", "10", "12", "-2"]);
  const table = within(figures).getByRole("table", { name: "逐笔费用与净盈亏", hidden: true });
  expect(
    within(table)
      .getAllByRole("cell", { hidden: true })
      .map(cell => cell.textContent),
  ).toEqual(["1", "SHFE · rb2610", "10", "12", "-2"]);
});

test("holding dates distinguish zero-day closed rounds, unknown evidence and still-open observation", () => {
  render(
    <TradeStatistics
      trades={{
        count: 2,
        winning: 0,
        losing: 0,
        gross: "0",
        fees: "0",
        net: "0",
        open_fees: "0",
        average_win: null,
        average_loss: null,
        payoff: null,
        by_contract: [],
        closed: [
          {
            venue: "SHFE",
            symbol: "rb2610",
            gross: "0",
            fees: "0",
            net: "0",
            holding: {
              opened_day: "2026-09-25",
              ended_day: "2026-09-25",
              calendar_days: 0,
              settlement_days: 0,
            },
          },
          { venue: "SHFE", symbol: "cu2610", gross: "0", fees: "0", net: "0", holding: null },
        ],
        holding_summary: {
          known: 1,
          unknown: 1,
          calendar_days: { minimum: 0, mean: 0, maximum: 0 },
          settlement_days: { minimum: 0, mean: 0, maximum: 0 },
        },
        open: [
          {
            venue: "SHFE",
            symbol: "ag2610",
            direction: "short",
            quantity: "1",
            fees: "0",
            as_of_day: "2026-09-28",
            holding: {
              opened_day: "2026-09-25",
              ended_day: "2026-09-28",
              calendar_days: 3,
              settlement_days: 1,
            },
          },
        ],
      }}
    />,
  );
  const closed = screen.getByRole("table", { name: "已平仓持有期", hidden: true });
  const rows = within(closed).getAllByRole("row", { hidden: true });
  expect(rows[1].textContent).toContain("2026-09-252026-09-2500");
  expect(rows[2].textContent).toContain("未知（无成交日期证据）");
  expect(screen.getByText("已平仓跨度已知 / 未知").parentElement!.textContent).toContain("1 / 1");
  const open = screen.getByRole("table", { name: "未平仓持有跨度", hidden: true });
  expect(open.textContent).toContain("2026-09-28");
  expect(open.textContent).toContain("空头");
  expect(screen.getByText(/同日均为0，不代表持有0分钟/)).toBeTruthy();
});
