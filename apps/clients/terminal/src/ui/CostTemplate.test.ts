import { expect, it } from "vitest";
import type { DatasetSelection, DatasetSeries } from "../bridge/client";
import { contractCostRequest } from "./CostTemplate";

const selected = (symbol: string, first_day: string) =>
  ({ venue: "SHFE", symbol, first_day }) as DatasetSelection;
const values = (margin: string) => ({
  margin_per_lot: margin,
  margin_rate: "0",
  open_fee: "2",
  close_today_fee: "3",
  close_yesterday_fee: "4",
  open_fee_rate: "0",
  close_today_fee_rate: "0",
  close_yesterday_fee_rate: "0",
});

it("sends costs once for each contract outside a series and once for each series", () => {
  // hc2610 is traded by itself; rb is a series of three months.
  const datasets = [
    selected("rb2610", "2026-06-02"),
    selected("hc2610", "2026-06-01"),
    selected("rb2701", "2026-08-20"),
    selected("rb2705", "2026-12-01"),
  ];
  const series: DatasetSeries[] = [
    { venue: "SHFE", product: "rb", symbols: ["rb2610", "rb2701", "rb2705"], rolls: [], terms: 0 },
  ];
  // What the form holds for a series is kept under its first month.
  const drafts = {
    "SHFE.rb2610": { ...values("300"), slippage_ticks: "2" },
    "SHFE.hc2610": values("400"),
  };
  expect(contractCostRequest(datasets, drafts, series)).toEqual([
    {
      venue: "SHFE",
      symbol: "hc2610",
      slippage_ticks: 1,
      cost_schedule: [
        { effective_from: "2026-06-01", source: "User-entered fixed costs", values: values("400") },
      ],
    },
    {
      venue: "SHFE",
      product: "rb",
      slippage_ticks: 2,
      // In force from the first month's first day, so it covers the later months.
      cost_schedule: [
        { effective_from: "2026-06-02", source: "User-entered fixed costs", values: values("300") },
      ],
    },
  ]);
});
