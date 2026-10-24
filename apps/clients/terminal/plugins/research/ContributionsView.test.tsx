import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import type { EvaluationContributions } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { ContributionsView } from "./ContributionsView";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);

test("fixed contract evidence preserves original precision, residual, open holdings and selected fill identity across paging", () => {
  const contract = (symbol: string): EvaluationContributions["contracts"][number] => ({
    venue: "SHFE",
    symbol,
    multiplier: "10",
    gross: "30.00000001",
    fees: "4",
    net: "26.00000001",
    final_position: { metric: "position", expected: "1", recorded: "1", residual: "0" },
    days: Array.from({ length: 51 }, (_, i) => ({
      source_index: i,
      trading_day: new Date(Date.UTC(2026, 0, 1 + i)).toISOString().slice(0, 10),
      timestamp_ns: "0",
      settlement: "-101.00000001",
      opening_quantity: i ? "1" : "0",
      quantity_change: i ? "0" : "1",
      closing_quantity: "1",
      gross: i ? "0" : "30.00000001",
      fees: i ? "0" : "4",
      checks: [],
      fills: i
        ? []
        : [
            {
              id: symbol + "-fill",
              order_id: symbol + "-order",
              buy: true,
              quantity: "1",
              price: "-100.00000001",
              fee: "4",
            },
          ],
    })),
  });
  const evidence: EvaluationContributions = {
    deposit: "1000",
    gross: "40.00000001",
    fees: "6",
    explained_net: "34.00000001",
    recorded_net: "35.00000001",
    residual: "1",
    reconciled: false,
    contracts: [contract("A"), { ...contract("B"), gross: "10", fees: "2", net: "8" }],
    days: Array.from({ length: 51 }, (_, i) => ({
      source_index: i,
      trading_day: new Date(Date.UTC(2026, 0, 1 + i)).toISOString().slice(0, 10),
      timestamp_ns: "0",
      gross: "0",
      fees: "0",
      expected_balance: "1034.00000001",
      recorded_balance: "1035.00000001",
      recorded_equity: "1035.00000001",
      cumulative_residual: "1",
      checks: [],
    })),
    final_checks: [],
  };
  const before = JSON.stringify(evidence);
  render(<ContributionsView evidence={evidence} currency="CNY" />);
  expect(screen.getByRole("status").textContent).toContain("存在未解释差额");
  const totals = screen.getByRole("table", { name: "全部原合约贡献合计" });
  expect(totals.textContent).toContain("26.00000001");
  expect(totals.textContent).toContain("SHFE/B");
  fireEvent.click(screen.getAllByRole("button", { name: "查看原成交与核对项" })[0]);
  let detail = screen.getByRole("region", { name: "所选合约原日结明细" });
  expect(detail.textContent).toContain("A-fill");
  expect(detail.textContent).toContain("-100.00000001");
  expect(detail.textContent).toContain("未记录观察时点");
  expect(detail.textContent).not.toContain("1970-");
  fireEvent.change(screen.getByRole("combobox", { name: "查看原贡献合约" }), {
    target: { value: "1" },
  });
  expect(screen.queryByRole("region", { name: "所选合约原日结明细" })).toBeNull();
  fireEvent.click(screen.getAllByRole("button", { name: "查看原成交与核对项" })[0]);
  detail = screen.getByRole("region", { name: "所选合约原日结明细" });
  expect(detail.textContent).toContain("B-fill");
  expect(detail.textContent).not.toContain("A-fill");
  fireEvent.click(screen.getByRole("button", { name: "下一页" }));
  const rows = within(screen.getByRole("table", { name: "原合约逐日贡献" })).getAllByRole("row");
  expect(rows).toHaveLength(2);
  expect(rows[1].textContent).toContain("50");
  expect(JSON.stringify(evidence)).toBe(before);
});
