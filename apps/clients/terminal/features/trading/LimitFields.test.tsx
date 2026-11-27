import { create } from "@bufbuild/protobuf";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { lab } from "../../src/bridge/lab";
import { RunsSchema, RunDetailSchema, RunState } from "../../src/generated/asterion/lab_pb";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { feature } from "./feature";
import { LimitFields } from "./LimitFields";
import type { LimitsDraft } from "./live-shared";

vi.mock("../../src/bridge/lab", () => ({ lab: vi.fn() }));
registerTerminalFeatures([feature]);

test("a backtest's limits fill the form and leave the rest as entered", async () => {
  vi.mocked(lab).mockImplementation(async operation =>
    operation.case === "listRecentRuns"
      ? (create(RunsSchema, {
          runs: [
            { id: "done", label: "趋势 2024", state: RunState.SUCCEEDED },
            { id: "failed", label: "失败的", state: RunState.FAILED },
          ],
        }) as never)
      : (create(RunDetailSchema, {
          record: JSON.stringify({
            risk: {
              max_order_lots: "3",
              max_contract_lots: "6",
              max_gross_lots: "12",
              max_working_orders: 4,
              max_margin_ratio: "0.35",
            },
          }),
        }) as never),
  );
  const change = vi.fn();
  const limits = { max_price_deviation: "2", max_daily_loss: "5000" } as LimitsDraft;
  render(<LimitFields limits={limits} change={change} />);
  const source = await screen.findByRole("combobox", { name: "从回测带入" });
  expect(screen.queryByRole("option", { name: "失败的" })).toBeNull();
  fireEvent.change(source, { target: { value: "done" } });
  await waitFor(() =>
    expect(change).toHaveBeenCalledWith({
      max_price_deviation: "2",
      max_daily_loss: "5000",
      max_order_quantity: "3",
      max_contract_quantity: "6",
      max_gross_quantity: "12",
      max_working_orders: "4",
      max_margin_ratio: "35",
    }),
  );
});

test("cross-field risk constraints surface inline errors and simulation preset fills conservative limits", () => {
  vi.mocked(lab).mockRejectedValue(new Error("offline"));
  const change = vi.fn();
  const invalid = {
    max_order_quantity: "5",
    max_contract_quantity: "12",
    max_gross_quantity: "10",
    max_working_orders: "4",
    max_price_deviation: "2",
    max_daily_loss: "",
    max_drawdown: "",
    max_margin_ratio: "",
    max_daily_cancels_per_contract: "",
  };
  render(<LimitFields limits={invalid} change={change} offerSimPreset />);
  expect(screen.getByText("单合约持仓上限不能大于账户总持仓上限")).toBeTruthy();
  expect(screen.getByRole("textbox", { name: "单合约持仓上限" }).getAttribute("aria-invalid")).toBe(
    "true",
  );
  fireEvent.click(screen.getByRole("button", { name: "填入轻仓仿真参考" }));
  expect(change).toHaveBeenCalledWith(
    expect.objectContaining({
      max_order_quantity: "1",
      max_contract_quantity: "2",
      max_gross_quantity: "5",
      max_working_orders: "5",
      max_price_deviation: "2",
    }),
  );
});

test("optional limits start collapsed when requested and expand once any optional value is set", () => {
  vi.mocked(lab).mockRejectedValue(new Error("offline"));
  const empty = {
    max_order_quantity: "",
    max_contract_quantity: "",
    max_gross_quantity: "",
    max_working_orders: "",
    max_price_deviation: "",
    max_daily_loss: "",
    max_drawdown: "",
    max_margin_ratio: "",
    max_daily_cancels_per_contract: "",
  };
  const { container, rerender } = render(
    <LimitFields limits={empty} change={vi.fn()} collapseOptional />,
  );
  const details = container.querySelector("details")!;
  expect(details.open).toBe(false);
  rerender(
    <LimitFields limits={{ ...empty, max_margin_ratio: "35" }} change={vi.fn()} collapseOptional />,
  );
  expect(details.open).toBe(true);
  expect(screen.getByText("已设 1 项")).toBeTruthy();
});
