import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type { BacktestReconciliation } from "../../src/bridge/client";
import { AccountReconciliation } from "./AccountReconciliation";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const audit = (): BacktestReconciliation => ({
  state: "available",
  model: {
    venue: "SHFE",
    symbol: "rb2401",
    deposit: "1000",
    multiplier: "10",
    cost_source: "explicit assumption",
    cost_effective_from: "2024-01-01",
    margin_per_lot: "50",
    margin_rate: "0.1",
    signed_notional: "1070",
    gross_notional: "1070",
  },
  final_checks: [
    { metric: "margin", expected: "157", recorded: "157", difference: "0", matches: true },
  ],
  days: [
    {
      day: "2024-01-02",
      opening_quantity: "0",
      quantity_change: "1",
      closing_quantity: "1",
      settlement: "107",
      opening_balance: "1000",
      gross: "70",
      fees: "1",
      expected_balance: "1069",
      recorded_balance: "1069",
      fills: [
        { id: "fill-1", order_id: "order-1", side: "buy", quantity: "1", price: "100", fee: "1" },
      ],
      checks: [
        { metric: "balance", expected: "1069", recorded: "1069", difference: "0", matches: true },
      ],
    },
  ],
});
test("fixed model and daily fill evidence explain arithmetic without asserting historical validity", () => {
  render(<AccountReconciliation audit={audit()} />);
  expect(screen.getByText(/所列算术核对一致/)).toBeTruthy();
  expect(screen.getByText(/历史保证金、乘数及规格适用性未在此证实/)).toBeTruthy();
  expect(screen.getByText(/保证金是资金占用，不从权益扣除/)).toBeTruthy();
  expect(screen.getByText("fill-1")).toBeTruthy();
  expect(screen.getByText("order-1")).toBeTruthy();
});
test("nonzero differences are actionable and remain separate from model assumptions", () => {
  const value = audit();
  value.final_checks[0] = {
    metric: "margin",
    expected: "157",
    recorded: "158",
    difference: "1",
    matches: false,
  };
  render(<AccountReconciliation audit={value} />);
  expect(screen.getByText(/存在差异：查看非零差额/)).toBeTruthy();
  expect(screen.getByText("1 · 有差异")).toBeTruthy();
});
test("missing dates and unsupported inputs are unknown rather than matching zero", () => {
  const { rerender } = render(
    <AccountReconciliation audit={{ state: "missing_fill_days", days: [], final_checks: [] }} />,
  );
  expect(screen.getByText(/逐日核对未知/)).toBeTruthy();
  expect(screen.queryByText(/所列算术核对一致/)).toBeNull();
  rerender(
    <AccountReconciliation audit={{ state: "unsupported_input", days: [], final_checks: [] }} />,
  );
  expect(screen.getByText(/其他输入不据此判断一致或不一致/)).toBeTruthy();
});

test("selecting a settlement day shows that day’s evidence outside the scrollable summary table", () => {
  const value = audit();
  value.days.push({ ...value.days[0], day: "2024-01-03", fills: [] });
  render(<AccountReconciliation audit={value} />);
  expect(screen.getByText("fill-1")).toBeTruthy();
  fireEvent.click(screen.getByText("2024-01-03").closest("tr")!.querySelector("button")!);
  expect(screen.queryByText("fill-1")).toBeNull();
  expect(screen.getByText("2024-01-03 · 当日核对证据")).toBeTruthy();
});
