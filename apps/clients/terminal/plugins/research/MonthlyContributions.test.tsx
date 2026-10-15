import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import type { BacktestMonth } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { setLocale } from "../../src/i18n";
import { MonthlyContributions } from "./MonthlyContributions";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);

const month: BacktestMonth = {
  first_day: "2026-01-29",
  last_day: "2026-01-30",
  trading_days: 2,
  opening_equity: "100000",
  closing_equity: "99999.99999999",
  gross: "1",
  fees: "1.00000001",
  net: "-0.00000001",
  return_contribution: -0.0000000000001,
};

test("monthly attribution preserves exact money and the recorded partial-month range", () => {
  render(<MonthlyContributions months={[month]} />);
  fireEvent.click(screen.getByText("按月收益贡献"));
  const row = within(screen.getByRole("table", { name: "按月收益贡献" })).getAllByRole("row")[1];
  expect(
    within(row)
      .getAllByRole("cell")
      .map(cell => cell.textContent),
  ).toEqual([
    "2026-01",
    "2026-01-29 – 2026-01-30",
    "2",
    "100000",
    "99999.99999999",
    "1",
    "1.00000001",
    "-0.00000001",
    "-0.00",
  ]);
  expect(screen.getByText(/不补齐缺失月份/).textContent).toContain("仍含滑点影响");
  expect(screen.getByText(/可相加的总收益率贡献/).textContent).toContain("包含训练段");
});

test("long results paginate in English without converting contributions to shares of profit", () => {
  setLocale("en-US");
  const months = Array.from({ length: 25 }, (_, index) => ({
    ...month,
    first_day: `${2024 + Math.floor(index / 12)}-${String((index % 12) + 1).padStart(2, "0")}-02`,
    last_day: `${2024 + Math.floor(index / 12)}-${String((index % 12) + 1).padStart(2, "0")}-02`,
    return_contribution: -1.1,
  }));
  render(<MonthlyContributions months={months} />);
  fireEvent.click(screen.getByText("Monthly Return Contributions"));
  const table = screen.getByRole("table", { name: "Monthly Return Contributions" });
  expect(within(table).getAllByRole("row")).toHaveLength(25);
  const next = screen.getByRole("button", { name: "Next page" });
  fireEvent.click(next);
  expect(within(table).getAllByRole("row")).toHaveLength(2);
  expect(within(table).getByText("2026-01")).toBeTruthy();
  expect(within(table).getByText("-110.00")).toBeTruthy();
  expect((next as HTMLButtonElement).disabled).toBe(true);
});
