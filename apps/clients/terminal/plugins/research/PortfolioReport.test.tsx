import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { setLocale } from "../../src/i18n";
import type { PortfolioAnalysis } from "../../src/bridge/client";
import { PortfolioReport } from "./PortfolioReport";
import { plugin } from "./plugin";
vi.mock("@asterion/client-ui/PriceChart", () => ({
  PriceChart: ({ label }: { label: string }) => <div role="img" aria-label={label} />,
}));
registerTerminalPlugins([plugin]);
const sources = [
  { id: "low-risk", result_digest: "a".repeat(64) },
  { id: "high-risk", result_digest: "b".repeat(64) },
];
function report(): PortfolioAnalysis {
  return {
    polars_version: "2.0.0",
    skfolio_version: "1.9.0",
    training_end: "2026-07-30",
    training_begin: "2026-07-01",
    training_last: "2026-07-30",
    training_observations: 30,
    weights: sources.map((source, i) => ({
      task_id: source.id,
      result_sha256: source.result_digest,
      weight: i ? 0.2 : 0.8,
      source_observations: 64,
    })),
    validation: Array.from({ length: 30 }, (_, i) => ({
      trading_day: `2026-08-${String(i + 1).padStart(2, "0")}`,
      optimized_return: 0.002,
      equal_weight_return: 0.001,
    })),
    optimized_total_return: 0.06,
    equal_weight_total_return: 0.03,
    optimized_annual_volatility: 0.04,
    equal_weight_annual_volatility: 0.08,
  };
}
test("requires an explicit boundary, sends verified identities, and shows allocation evidence", async () => {
  const query = vi.fn(async () => ({ ...readySnapshot(), portfolio_report: report() }));
  render(<PortfolioReport sources={sources} busy={false} query={query} />);
  const button = screen.getByRole("button", { name: "分析回测组合" }) as HTMLButtonElement;
  expect(button.disabled).toBe(true);
  fireEvent.change(screen.getByLabelText("组合训练截止日（含）"), {
    target: { value: "2026-07-30" },
  });
  fireEvent.click(button);
  await screen.findByRole("img", { name: "组合验证收益率" });
  expect(query).toHaveBeenCalledWith("task.portfolio.analyze", {
    expected: sources,
    training_end: "2026-07-30",
  });
  const weights = screen.getByRole("table", { name: "组合配比与来源" });
  const low = within(weights).getByText("low-risk").closest("tr")!;
  expect(low.textContent).toContain("80.0000%");
  expect(within(low).getByText("4")).toBeTruthy();
  expect(screen.getByText(/原策略已回看/).textContent).toContain("未计组合再平衡费用");
  expect(screen.getByText(/Polars 2.0.0/)).toBeTruthy();
  fireEvent.change(screen.getByLabelText("组合训练截止日（含）"), {
    target: { value: "2026-07-31" },
  });
  expect(screen.queryByRole("table")).toBeNull();
});

test("rejects a mismatched result digest in English", async () => {
  setLocale("en-US");
  const value = report();
  value.weights[0].result_sha256 = "c".repeat(64);
  const query = vi.fn(async () => ({ ...readySnapshot(), portfolio_report: value }));
  render(<PortfolioReport sources={sources} busy={false} query={query} />);
  fireEvent.change(screen.getByLabelText("Portfolio Training End (Inclusive)"), {
    target: { value: "2026-07-30" },
  });
  fireEvent.click(screen.getByRole("button", { name: "Analyze Backtest Portfolio" }));
  await screen.findByText(
    "The portfolio report does not match the current experiments or training boundary",
  );
  expect(screen.queryByRole("table")).toBeNull();
});
