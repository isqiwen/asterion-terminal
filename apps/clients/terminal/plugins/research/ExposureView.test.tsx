import { render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import type { EvaluationExposure } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { ExposureView } from "./ExposureView";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
function originalSlice(): EvaluationExposure {
  return {
    source_index: 4,
    trading_day: "2026-09-25",
    timestamp_ns: "1790300000000000000",
    balance: "1000",
    funds: { margin: "200", available: "800", frozen: "0" },
    covered_net_notional: "0",
    covered_gross_notional: "4000",
    net_to_equity: { state: "AVAILABLE", value: 0 },
    gross_to_equity: { state: "AVAILABLE", value: 4 },
    margin_to_equity: { state: "AVAILABLE", value: 0.2 },
    positions: [
      {
        source_contract_index: 0,
        venue: "SHFE",
        symbol: "rb2610",
        product: "rb",
        delivery_month: "2026-10",
        price: "100",
        multiplier: "10",
        quantity_increment: "1",
        net_quantity: "0",
        gross_quantity: "4",
        margin: "200",
        long_quantity: "2",
        short_quantity: "2",
        net_notional: "0",
        gross_notional: "4000",
        net_to_equity: { state: "AVAILABLE", value: 0 },
        gross_to_equity: { state: "AVAILABLE", value: 4 },
        margin_to_equity: { state: "AVAILABLE", value: 0.2 },
        covered_gross_share: { state: "AVAILABLE", value: 1 },
      },
    ],
  };
}
function values() {
  return within(screen.getByRole("table", { name: "原合约敞口与单位证据" })).getAllByRole("row")[1];
}
test("recorded net zero keeps gross, derived sides, actual funds and their distinct units", () => {
  render(<ExposureView slice={originalSlice()} equity="1000" currency="CNY" />);
  expect(screen.getByText("所列净名义金额").nextElementSibling?.textContent).toBe("0 CNY");
  expect(screen.getByText("所列总名义金额").nextElementSibling?.textContent).toBe("4000 CNY");
  expect(screen.getByText("原可用资金").nextElementSibling?.textContent).toBe("800 CNY");
  const cells = within(values()).getAllByRole("cell");
  expect(cells[1].textContent).toContain("SHFE/rb2610");
  expect(cells[5].textContent).toBe("0");
  expect(cells[6].textContent).toBe("4");
  expect(cells[7].textContent).toBe("2");
  expect(cells[8].textContent).toBe("2");
  expect(cells[10].textContent).toBe("4000 CNY");
  expect(cells[11].textContent).toBe("200 CNY");
  expect(screen.getByText(/双边总额、保证金、基差和退出风险不会因此消失/)).toBeTruthy();
});
test("missing original gross and funds are not reconstructed from zero net or original cash balance", () => {
  const slice = originalSlice();
  slice.funds = null;
  slice.covered_gross_notional = null;
  slice.gross_to_equity = { state: "GROSS_NOT_RECORDED", value: null };
  slice.margin_to_equity = { state: "MARGIN_NOT_RECORDED", value: null };
  const row = slice.positions[0];
  row.gross_quantity = null;
  row.margin = null;
  row.long_quantity = null;
  row.short_quantity = null;
  row.gross_notional = null;
  row.gross_to_equity = { state: "GROSS_NOT_RECORDED", value: null };
  row.margin_to_equity = { state: "MARGIN_NOT_RECORDED", value: null };
  row.covered_gross_share = { state: "GROSS_NOT_RECORDED", value: null };
  render(<ExposureView slice={slice} equity="1000" currency="CNY" />);
  expect(screen.getByText("所列总名义金额").nextElementSibling?.textContent).toBe("未记录");
  expect(screen.getByText("原可用资金").nextElementSibling?.textContent).toBe("未记录");
  const cells = within(values()).getAllByRole("cell");
  expect(cells[5].textContent).toBe("0");
  for (const index of [6, 7, 8, 10, 11]) expect(cells[index].textContent).toBe("未记录");
  expect(cells[13].textContent).toBe("原总手数未记录");
  expect(cells[14].textContent).toBe("原保证金未记录");
  expect(screen.getByText(/不从余额、费率或其他时点重构/)).toBeTruthy();
});
test("nonpositive equity keeps recorded money while concentration stays independent from equity ratios", () => {
  const slice = originalSlice();
  slice.net_to_equity = { state: "NON_POSITIVE_EQUITY_PATH", value: null };
  slice.gross_to_equity = { state: "NON_POSITIVE_EQUITY_PATH", value: null };
  slice.margin_to_equity = { state: "NON_POSITIVE_EQUITY_PATH", value: null };
  const row = slice.positions[0];
  row.net_to_equity = { state: "NON_POSITIVE_EQUITY_PATH", value: null };
  row.gross_to_equity = { state: "NON_POSITIVE_EQUITY_PATH", value: null };
  row.margin_to_equity = { state: "NON_POSITIVE_EQUITY_PATH", value: null };
  render(<ExposureView slice={slice} equity="-1" currency="CNY" />);
  expect(screen.getByText("原权益").nextElementSibling?.textContent).toBe("-1 CNY");
  expect(screen.getByText("所列总名义金额").nextElementSibling?.textContent).toBe("4000 CNY");
  const cells = within(values()).getAllByRole("cell");
  expect(cells[12].textContent).toBe("区间中存在非正权益");
  expect(cells[15].textContent).toBe("100.00%");
});
