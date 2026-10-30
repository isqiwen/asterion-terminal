import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import type {
  PortfolioExposure,
  PortfolioBudgetReview,
  Snapshot,
  TaskRecord,
} from "../../src/bridge/client";
import { PortfolioBudget } from "./PortfolioBudget";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
const facts: PortfolioExposure = {
  scope: "final_saved_account",
  settlement_path: { scope: "recorded_settlement_points", points: [] },
  contracts: [],
  net_notional: "0",
  gross_notional: "2200",
  gross_quantity: "2",
  model_margin: "200",
  recorded_margin: "200",
  margin_difference: "0",
  balance: "99786",
  equity: "99786",
  available: "99586",
  frozen: "0",
  risk: {
    max_order_quantity: "100",
    max_gross_quantity: "100",
    max_working_orders: 100,
    final_lot_headroom: "98",
  },
};
const source = {
  id: "original",
  input_sha256: "a".repeat(64),
  task: { result_digest: "b".repeat(64) } as TaskRecord,
  portfolio: facts,
};
const review: PortfolioBudgetReview = {
  scope: "saved_account_and_settlement_points_review_only",
  settlement_checks: [],
  settlement_summary: {
    gross_notional: { checked_points: 0, unknown_points: 0, exceeded_points: 0 },
    margin: { checked_points: 0, unknown_points: 0, exceeded_points: null },
  },
  source: {
    task_id: source.id,
    input_sha256: source.input_sha256,
    result_digest: source.task.result_digest,
    task_instance: "isolated",
  },
  limits: { gross_notional: "2100", margin: null },
  checks: {
    gross_notional: { used: "2200", limit: "2100", headroom: "-100", state: "exceeded" },
    margin: { used: "200", limit: null, headroom: null, state: "unset" },
  },
  portfolio: facts,
};
test("budgets start unset, distinct from original execution limits and allocation weights", () => {
  const query = vi.fn();
  render(<PortfolioBudget result={source} busy={false} query={query} />);
  expect((screen.getByLabelText("总名义限额（期末与结算点复核）") as HTMLInputElement).value).toBe(
    "",
  );
  expect((screen.getByText("复核记录点预算") as HTMLButtonElement).disabled).toBe(true);
  expect(screen.getByText(/不是策略目标权重/)).toBeTruthy();
  expect(screen.getByText(/不证明全程未超限/)).toBeTruthy();
  expect(query).not.toHaveBeenCalled();
});
test("explicit exact strings are reviewed against fixed identity and editing invalidates the receipt", async () => {
  const before = JSON.stringify(source);
  const query = vi.fn(async () => ({ portfolio_budget_review: review }) as Snapshot);
  render(<PortfolioBudget result={source} busy={false} query={query} />);
  const input = screen.getByLabelText("总名义限额（期末与结算点复核）");
  fireEvent.change(input, { target: { value: "2100" } });
  fireEvent.click(screen.getByText("复核记录点预算"));
  await waitFor(() => expect(screen.getByText("复核超限（非执行拦截）")).toBeTruthy());
  expect(query).toHaveBeenCalledWith("backtest.budget.review", {
    id: source.id,
    input_sha256: source.input_sha256,
    result_digest: source.task.result_digest,
    limits: { gross_notional: "2100", margin: null },
  });
  expect(screen.getAllByText("未设置").length).toBeGreaterThan(0);
  fireEvent.change(input, { target: { value: "2200" } });
  expect(screen.queryByText("复核超限（非执行拦截）")).toBeNull();
  expect(JSON.stringify(source)).toBe(before);
});
test("pending review cannot race with editable thresholds", async () => {
  let done!: (reply: Snapshot) => void;
  const query = vi.fn(
    () =>
      new Promise<Snapshot>(resolve => {
        done = resolve;
      }),
  );
  render(<PortfolioBudget result={source} busy={false} query={query} />);
  const input = screen.getByLabelText("总名义限额（期末与结算点复核）");
  fireEvent.change(input, { target: { value: "0" } });
  fireEvent.click(screen.getByText("复核记录点预算"));
  expect((input as HTMLInputElement).disabled).toBe(true);
  done({ portfolio_budget_review: review } as Snapshot);
  await waitFor(() => expect((input as HTMLInputElement).disabled).toBe(false));
});

test("recorded-point summary separates earlier excess and unknown observations from the final snapshot", async () => {
  const point = {
    day: "2026-09-21",
    net_notional: "0",
    gross_notional: "2400",
    model_margin: "200",
    balance: "10000",
    equity: "10000",
    available: "9800",
    frozen: "0",
  };
  const missing = {
    ...point,
    day: "2026-09-22",
    gross_notional: null,
    model_margin: null,
    available: null,
    frozen: null,
  };
  const result = {
    ...source,
    portfolio: {
      ...facts,
      settlement_path: { scope: "recorded_settlement_points" as const, points: [point, missing] },
    },
  };
  const response = {
    ...review,
    portfolio: result.portfolio,
    checks: {
      ...review.checks,
      gross_notional: { used: "2200", limit: "2300", headroom: "100", state: "within" as const },
    },
    settlement_checks: [
      {
        ...point,
        checks: {
          gross_notional: {
            used: "2400",
            limit: "2300",
            headroom: "-100",
            state: "exceeded" as const,
          },
          margin: { used: "200", limit: null, headroom: null, state: "unset" as const },
        },
      },
      {
        ...missing,
        checks: {
          gross_notional: { used: null, limit: "2300", headroom: null, state: "unknown" as const },
          margin: { used: null, limit: null, headroom: null, state: "unset" as const },
        },
      },
    ],
    settlement_summary: {
      gross_notional: { checked_points: 1, unknown_points: 1, exceeded_points: 1 },
      margin: { checked_points: 0, unknown_points: 1, exceeded_points: null },
    },
  };
  const query = vi.fn(async () => ({ portfolio_budget_review: response }) as Snapshot);
  render(<PortfolioBudget result={result} busy={false} query={query} />);
  fireEvent.change(screen.getByLabelText("总名义限额（期末与结算点复核）"), {
    target: { value: "2300" },
  });
  fireEvent.click(screen.getByText("复核记录点预算"));
  await waitFor(() => expect(screen.getByText("未知（缺少结算占用观测）")).toBeTruthy());
  expect(screen.getByText(/跨品种代数净额为零不代表风险对冲或风险为零/)).toBeTruthy();
  expect(screen.getByText("复核超限（非执行拦截）")).toBeTruthy();
  expect(screen.getAllByText("复核范围内（非执行许可）").length).toBeGreaterThan(0);
  expect(screen.getByText(/总名义结算点复核/).textContent).toContain("已知超限点 1");
  expect(screen.getByText(/总名义结算点复核/).textContent).toContain("未知点 1");
});
