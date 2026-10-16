import { useState } from "react";
import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import type { SettlementMargin } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { setLocale } from "../../src/i18n";
import { plugin } from "./plugin";
import {
  SettlementMarginEditor,
  SettlementMarginDetails,
  SettlementMarginResults,
} from "./SettlementMargins";
registerTerminalPlugins([plugin]);

function Draft({ supported }: { supported: boolean }) {
  const [events, setEvents] = useState<SettlementMargin[]>([
    { trading_day: "2026-09-28", rate: "0.18000001", source: "explicit model assumption" },
  ]);
  return (
    <>
      <SettlementMarginEditor events={events} supported={supported} onChange={setEvents} />
      <SettlementMarginDetails events={events} />
    </>
  );
}

test("margin draft preserves decimal input and requires explicit removal when its scope changes", () => {
  const view = render(<Draft supported />);
  fireEvent.change(screen.getByLabelText("结算保证金比例"), { target: { value: "0.17000001" } });
  expect(screen.getByRole("table", { name: "结算保证金参数" }).textContent).toContain("0.17000001");
  view.rerender(<Draft supported={false} />);
  expect(screen.getByRole("alert").textContent).toContain("删除调整");
  expect((screen.getByRole("button", { name: "添加结算调整" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  expect((screen.getByLabelText("结算保证金比例") as HTMLInputElement).value).toBe("0.17000001");
  fireEvent.click(screen.getByRole("button", { name: "删除调整" }));
  expect(screen.queryByRole("table")).toBeNull();
});

test("English result presents recorded before and after amounts without rounding or liquidation claims", () => {
  setLocale("en-US");
  render(
    <SettlementMarginResults
      settlements={[
        {
          trading_day: "2026-09-28",
          timestamp_ns: "1",
          balance: "158",
          equity: "158",
          realized: "10",
          fees: "2",
          contracts: [],
          margin_revaluation: {
            previous_rate: "0.14",
            rate: "0.18000001",
            margin_before: "140",
            available_before: "18",
          },
          occupancy: { margin: "180.00001", available: "-22.00001", frozen: "0" },
        },
      ]}
    />,
  );
  fireEvent.click(screen.getByText("Settlement Margin Impact"));
  const row = within(screen.getByRole("table")).getAllByRole("row")[1];
  expect(
    within(row)
      .getAllByRole("cell")
      .map(cell => cell.textContent),
  ).toEqual(["2026-09-28", "0.14", "0.18000001", "140", "180.00001", "18", "-22.00001"]);
  expect(screen.getByText(/triggers no automatic liquidation/)).toBeTruthy();
});
