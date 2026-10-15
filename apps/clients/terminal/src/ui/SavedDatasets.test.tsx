import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import type { SavedNamedDataset, Snapshot } from "../bridge/client";
import { SavedDatasets } from "./SavedDatasets";

const daily = {
  snapshot: "b".repeat(64),
  daily_source: "tushare.fut_daily.v3",
  calendar_source: "tushare.trade_cal.v1",
  contracts_source: "tushare.fut_basic.v1",
};
const saved: SavedNamedDataset = {
  id: "a".repeat(64),
  name: "金属组合",
  selections: [
    {
      source_dataset_ids: [],
      daily,
      begin_day: "2026-01-05",
      end_day: "2026-09-30",
      contract: "SHFE/cu/2026-10",
      revision: "c".repeat(64),
    },
    {
      source_dataset_ids: ["d".repeat(64)],
      daily,
      begin_day: "",
      end_day: "",
      contract: "SHFE/rb/2026-10",
      revision: "e".repeat(64),
    },
  ],
};

test("a saved dataset names each contract as history does, with its days", async () => {
  render(
    <SavedDatasets
      snapshot={{ data: { online: true }, datasets: [] } as unknown as Snapshot}
      busy={false}
      revision={0}
      trade={async () => {}}
      query={async () => ({ saved_datasets: [saved] }) as unknown as Snapshot}
    />,
  );
  fireEvent.change(await screen.findByRole("combobox", { name: "已保存数据集" }), {
    target: { value: saved.id },
  });
  expect(
    await screen.findByText(
      "SHFE/cu/2026-10 · 2026-01-05 — 2026-09-30； SHFE/rb/2026-10 · 全部交易日",
    ),
  ).toBeTruthy();
});
