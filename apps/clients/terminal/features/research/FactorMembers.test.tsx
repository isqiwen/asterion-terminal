import { render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import type { FactorExperiment, FactorResult } from "../../src/bridge/client";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { FactorMembers } from "./FactorMembers";
import { feature } from "./feature";

registerTerminalFeatures([feature]);
test("member evidence preserves grouping order, identities and zero labels without inventing missing members", () => {
  const experiment: Pick<FactorExperiment, "series" | "factor"> = {
    factor: "momentum",
    series: ["SHFE/rb", "SHFE/cu", "SHFE/al"].map(instrument => ({
      kind: "table",
      data: {
        instrument,
        snapshot: "a".repeat(64),
        daily_source: "fixture.daily",
        calendar_source: "fixture.calendar",
        begin_day: "2026-01-01",
        end_day: "2026-01-03",
        acquired_at_ns: "1",
        months: [],
        rolls: [],
        terms: 0,
        count: 3,
      },
    })),
  };
  const section: FactorResult["cross_sections"][number] = {
    event_index: 1,
    observed: "2026-01-02",
    label: "2026-01-03",
    pearson: null,
    spearman: null,
    series: 2,
    members: [
      { series_index: 2, value: 0.1, forward_return: 0 },
      { series_index: 0, value: -0.01, forward_return: -0.05 },
    ],
  };
  const { rerender } = render(
    <FactorMembers experiment={experiment} section={section} lookback={5} />,
  );
  const table = screen.getByRole("table", { name: "截面成员明细" });
  expect(
    within(table)
      .getAllByRole("cell")
      .map(cell => cell.textContent),
  ).toEqual(["1", "SHFE/al", "10.0000%", "0.0000%", "2", "SHFE/rb", "-1.0000%", "-5.0000%"]);
  expect(screen.queryByText("SHFE/cu")).toBeNull();
  expect(screen.getByText(/2026-01-02 → 2026-01-03/).textContent).toContain("实际窗口: 5");
  rerender(
    <FactorMembers
      experiment={experiment}
      section={{ ...section, event_index: 2, observed: "2026-01-03", members: [], series: 0 }}
      lookback={2}
    />,
  );
  expect(screen.queryByRole("table")).toBeNull();
  expect(screen.getByText("此观测没有实际参与成员")).toBeTruthy();
});
