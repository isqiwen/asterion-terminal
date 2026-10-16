import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { setLocale } from "../../src/i18n";
import { QuantStatsReport } from "./QuantStatsReport";
import { plugin } from "./plugin";
vi.mock("@asterion/client-ui/PriceChart", () => ({
  PriceChart: ({ label }: { label: string }) => <div role="img" aria-label={label} />,
}));
registerTerminalPlugins([plugin]);

test("requests the verified result identity and presents absent statistics and methodology", async () => {
  const query = vi.fn(async () => ({
    ...readySnapshot(),
    quantstats_report: {
      task_id: "test",
      result_sha256: "a".repeat(64),
      library_version: "0.0.86",
      periods_per_year: 252,
      returns: [{ trading_day: "2026-09-01", value: -0.02 }],
      metrics: {
        sortino: null,
        omega: 1.2,
        tail_ratio: null,
        ulcer_index: 0.02,
        value_at_risk: -0.03,
        conditional_value_at_risk: -0.04,
      },
    },
  }));
  render(<QuantStatsReport id="test" resultDigest={"a".repeat(64)} busy={false} query={query} />);
  fireEvent.click(screen.getByRole("button", { name: "生成 QuantStats 报告" }));
  await screen.findByRole("img", { name: "日收益率" });
  expect(query).toHaveBeenCalledWith("task.quantstats.analyze", {
    id: "test",
    result_digest: "a".repeat(64),
  });
  expect(screen.getByText(/按全程实际结算权益/).textContent).toContain("包含训练段");
  expect(screen.getByText(/VaR 与 CVaR/).textContent).toContain("正态分布");
  expect(within(screen.getByText("索提诺比率").parentElement!).getByText("—")).toBeTruthy();
  expect(screen.getByText("-4.000%")).toBeTruthy();
});

test("rejects a report for another result and renders the error in English", async () => {
  setLocale("en-US");
  const query = vi.fn(async () => readySnapshot());
  render(<QuantStatsReport id="test" resultDigest={"b".repeat(64)} busy={false} query={query} />);
  fireEvent.click(screen.getByRole("button", { name: "Generate QuantStats Report" }));
  await screen.findByText("The QuantStats report does not match the current backtest");
  expect(screen.queryByRole("img")).toBeNull();
});
