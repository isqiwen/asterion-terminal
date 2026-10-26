import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type { PerformanceFigures } from "../../src/bridge/client";
import { Figures } from "./Performance";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);

const raw: PerformanceFigures = {
  trading_days: 3,
  total_return: 0.02,
  max_drawdown: 0.01,
  winning_days: 2 / 3,
  annual_return: null,
  annual_volatility: null,
  sharpe: null,
  calmar: null,
};

test("missing method evidence remains unavailable while original monetary facts stay visible", () => {
  render(<Figures label="原始绩效" value={raw} />);
  const region = screen.getByRole("region", { name: "原始绩效" });
  expect(region.textContent).toContain("2.00%");
  expect(region.textContent).toContain("本记录未提供明确绩效方法上下文。");
  expect(region.textContent).not.toContain("明确零无风险");
});

test("read-time figures preserve supplied values and distinguish absent risk-free from explicit zero", () => {
  const fixed: PerformanceFigures = {
    ...raw,
    annual_return: null,
    annual_volatility: 0.45,
    calculation: {
      begin_ns: "1767571200000000000",
      end_ns: "1767744000000000000",
      calendar_year_days: 365.2425,
      annual_periods: 252,
      risk_free_annual: null,
      frequency: "daily_settlement",
      opening_return_included: false,
      return_periods: 2,
      short_calendar_span: true,
      derived_on_read: true,
      unavailable: {
        annual_return: "observation_shorter_than_one_year",
        annual_volatility: null,
        sharpe: "risk_free_not_configured",
        calmar: "no_drawdown",
      },
    },
  };
  const before = JSON.stringify(fixed);
  render(<Figures label="固定绩效" value={fixed} />);
  const region = screen.getByRole("region", { name: "固定绩效" });
  fireEvent.click(within(region).getByText("绩效方法与不可计算原因"));
  expect(region.textContent).toContain("45.00%");
  expect(region.textContent).toContain("无风险收益未配置");
  expect(region.textContent).toContain("无回撤，卡玛分母为零");
  expect(region.textContent).toContain("开端短周期");
  expect(region.textContent).toContain("观测不足一年，不年化");
  expect(region.textContent).not.toContain("明确零无风险");
  expect(JSON.stringify(fixed)).toBe(before);
});
