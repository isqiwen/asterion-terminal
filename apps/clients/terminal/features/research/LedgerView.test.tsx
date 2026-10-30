import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import type { EvaluationLedger, EvaluationLedgerCheck } from "../../src/bridge/client";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { LedgerView } from "./LedgerView";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
function evidence(): EvaluationLedger {
  const check = (
    metric: EvaluationLedgerCheck["metric"],
    expected = "0",
    recorded = expected,
    residual = "0",
  ): EvaluationLedgerCheck => ({ metric, expected, recorded, residual });
  return {
    venue: "SHFE",
    symbol: "fixture",
    multiplier: "1",
    deposit: "100",
    gross: "-2.25",
    fees: "0.75",
    explained_net: "-3",
    recorded_net: "-2",
    residual: "1",
    closing_net_quantity: "1",
    closing_gross_quantity: "1",
    reconciled: false,
    days: Array.from({ length: 51 }, (_, index) => ({
      source_index: index,
      trading_day: new Date(Date.UTC(2026, 0, 1 + index)).toISOString().slice(0, 10),
      timestamp_ns: "0",
      opening_quantity: index ? "1" : "0",
      quantity_change: index ? "0" : "1",
      closing_quantity: "1",
      opening_balance: index ? "97" : "100",
      gross: index ? "0" : "-2.25",
      fees: index ? "0" : "0.75",
      expected_balance: "97",
      recorded_balance: "98",
      recorded_equity: "98",
      cumulative_residual: "1",
      fills: index
        ? []
        : [
            {
              id: "fill-original",
              order_id: "order-original",
              buy: true,
              quantity: "1",
              price: "-1",
              fee: "0.75",
            },
          ],
      checks: [
        check("settlement", "-3.25"),
        check("position", "1"),
        check("fees", "0.75"),
        check("balance", "97", "98", "1"),
        check("equity", "97", "98", "1"),
        check("realized", "-2.25", "-1.25", "1"),
      ],
    })),
    final_checks: [
      check("position", "1"),
      check("fees", "0.75"),
      check("balance", "97", "98", "1"),
      check("equity", "97", "98", "1"),
      check("realized", "-2.25", "-1.25", "1"),
      check("unrealized"),
      check("frozen"),
      check("margin", "10"),
      check("available", "88"),
    ],
  };
}
test("fixed ledger preserves unexplained money, negative prices and open holdings without recalculating", () => {
  const ledger = evidence(),
    before = JSON.stringify(ledger);
  render(<LedgerView ledger={ledger} currency="CNY" />);
  expect(screen.getByRole("status").textContent).toContain("存在未解释差额");
  const table = screen.getByRole("table", { name: "原日结毛变动费用与累计差额" });
  expect(within(table).getAllByRole("row")).toHaveLength(51);
  expect(table.textContent).toContain("-2.25");
  fireEvent.click(
    within(within(table).getAllByRole("row")[1]).getByRole("button", {
      name: "查看原成交与核对项",
    }),
  );
  expect(screen.getByRole("region", { name: "所选原日结明细" }).textContent).toContain(
    "未记录观察时点",
  );
  const fills = screen.getByRole("table", { name: "所选日结原成交" });
  expect(fills.textContent).toContain("fill-original");
  expect(fills.textContent).toContain("-1");
  expect(screen.getByText(/账上未结算估值为零不表示全部平仓/)).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "下一页" }));
  expect(within(table).getAllByRole("row")).toHaveLength(2);
  expect(within(table).getAllByRole("row")[1].textContent).toContain("50");
  expect(JSON.stringify(ledger)).toBe(before);
});
