import { render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import type { EvaluationDailyDrawdown } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { DailyDrawdownView } from "./DailyDrawdownView";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
function originalPath(): EvaluationDailyDrawdown {
  return {
    max_loss: "11",
    max_loss_source_index: 14,
    max_fraction: { state: "AVAILABLE", value: 0.1 },
    max_fraction_source_index: 14,
    points: [
      {
        source_index: 12,
        trading_day: "2026-09-21",
        timestamp_ns: "1789990000000000000",
        equity: "100",
        peak_source_index: 12,
        peak_equity: "100",
        loss: "0",
        fraction: { state: "AVAILABLE", value: 0 },
      },
      {
        source_index: 13,
        trading_day: "2026-09-22",
        timestamp_ns: null,
        equity: "110",
        peak_source_index: 13,
        peak_equity: "110",
        loss: "0",
        fraction: { state: "AVAILABLE", value: 0 },
      },
      {
        source_index: 14,
        trading_day: "2026-09-23",
        timestamp_ns: null,
        equity: "99",
        peak_source_index: 13,
        peak_equity: "110",
        loss: "11",
        fraction: { state: "AVAILABLE", value: 0.1 },
      },
      {
        source_index: 15,
        trading_day: "2026-09-24",
        timestamp_ns: null,
        equity: "105",
        peak_source_index: 13,
        peak_equity: "110",
        loss: "5",
        fraction: { state: "AVAILABLE", value: 0.045454545454545456 },
      },
    ],
    episodes: [
      {
        peak_source_index: 13,
        trough_source_index: 14,
        recovery_source_index: null,
        observed_end_source_index: 15,
        peak_to_trough_intervals: 1,
        peak_to_recovery_intervals: null,
        observed_intervals: 2,
      },
    ],
  };
}
test("daily loss view reads saved amounts and original indices without inventing recovery or UTC", () => {
  render(<DailyDrawdownView path={originalPath()} currency="CNY" />);
  const rows = within(screen.getByRole("table", { name: "日采样原行证据" })).getAllByRole("row");
  expect(rows).toHaveLength(5);
  const cells = within(rows[3]).getAllByRole("cell");
  expect(cells[0].textContent).toBe("14");
  expect(cells[2].textContent).toBe("未记录");
  expect(cells[3].textContent).toBe("99 CNY");
  expect(cells[6].textContent).toBe("11 CNY");
  const episode = within(screen.getByRole("table", { name: "日采样回撤片段" })).getAllByRole(
    "row",
  )[1];
  const episodeCells = within(episode).getAllByRole("cell");
  expect(episodeCells[2].textContent).toBe("尚未恢复");
  expect(episodeCells[4].textContent).toBe("尚未恢复");
  expect(episodeCells[5].textContent).toBe("2");
  expect(screen.getByRole("img", { name: "当前页日采样回撤曲线" })).toBeTruthy();
  expect(screen.getByText(/不代表盘中最大损失/)).toBeTruthy();
});
test("undefined saved fractions stay unavailable and disconnect the displayed curve", () => {
  const path = originalPath();
  path.max_loss = "111";
  path.max_fraction = { state: "NON_POSITIVE_EQUITY_PATH", value: null };
  path.max_fraction_source_index = null;
  path.points[2] = {
    ...path.points[2],
    equity: "-1",
    loss: "111",
    fraction: { state: "NON_POSITIVE_EQUITY_PATH", value: null },
  };
  render(<DailyDrawdownView path={path} currency="CNY" />);
  expect(screen.getByText(/非正权益使全路径比例覆盖不完整/)).toBeTruthy();
  const rows = within(screen.getByRole("table", { name: "日采样原行证据" })).getAllByRole("row");
  const cells = within(rows[3]).getAllByRole("cell");
  expect(cells[3].textContent).toBe("-1 CNY");
  expect(cells[7].textContent).toBe("非正权益，比例不适用");
  expect(
    screen.getByRole("img", { name: "当前页日采样回撤曲线" }).querySelectorAll("polyline"),
  ).toHaveLength(2);
});
