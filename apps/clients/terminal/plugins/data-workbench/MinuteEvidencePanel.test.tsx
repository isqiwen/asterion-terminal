import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { MinuteEvidence, Snapshot } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import type { TerminalContext } from "../contract";
import { MinuteEvidencePanel } from "./MinuteEvidencePanel";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
const ns = (text: string) => String(BigInt(Date.parse(text + "+08:00")) * 1000000n);
const evidence: MinuteEvidence = {
  dataset_id: "minute",
  begin_ns: ns("2026-09-01T00:00:00"),
  end_ns: ns("2026-11-30T23:59:59"),
  total_rows: 52,
  unknown_rows: 1,
  total_days: 51,
  offset: 0,
  limit: 50,
  days: Array.from({ length: 51 }, (_, index) => {
    const day = new Date(Date.UTC(2026, 8, 7 + index)).toISOString().slice(0, 10);
    return {
      trading_day: day,
      rows: 1,
      first_ns: ns(`${day}T09:01:00`),
      last_ns: ns(`${day}T09:01:00`),
    };
  }),
};
const reply = (page: MinuteEvidence): Snapshot => ({ ...readySnapshot(), minute_evidence: page });
test("observations preserve night labels, expose unknown rows, and page without declaring completeness", async () => {
  const night = {
    ...evidence.days[0],
    first_ns: ns("2026-09-04T21:01:00"),
    last_ns: ns("2026-09-04T21:01:00"),
  };
  const query = vi.fn(async (_method: string, params: Record<string, unknown>) => {
    const offset = Number(params.offset);
    return reply({
      ...evidence,
      offset,
      days: [night, ...evidence.days.slice(1)].slice(offset, offset + 50),
    });
  });
  render(<MinuteEvidencePanel id="minute" query={query} disabled={false} />);
  expect(query).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "按交易日检查" }));
  await screen.findByRole("table");
  expect(
    screen.getByText("共 52 条记录，51 个已标记交易日；1 条记录缺少交易日标记。"),
  ).toBeTruthy();
  const row = screen.getByText("2026-09-07").closest("tr")!;
  expect(row.textContent).toContain("2026/9/4 21:01:00");
  expect(within(row).getByText("未验证")).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "下一页" }));
  await waitFor(() => expect(screen.getByText("51–51 / 51")).toBeTruthy());
  expect(query).toHaveBeenLastCalledWith("data.minutes.evidence", {
    dataset_id: "minute",
    offset: 50,
    limit: 50,
  });
  expect(within(screen.getByRole("table")).getAllByRole("row")).toHaveLength(2);
});
test("a reply from the previous data service cannot populate its replacement view", async () => {
  let release!: (reply: Snapshot) => void;
  const pending = new Promise<Snapshot>(resolve => {
    release = resolve;
  });
  const oldQuery: TerminalContext["query"] = async () => pending;
  const nextQuery: TerminalContext["query"] = async () =>
    reply({
      ...evidence,
      total_rows: 0,
      total_days: 0,
      unknown_rows: 0,
      days: [],
    });
  const view = render(
    <MinuteEvidencePanel key="service-a" id="minute" query={oldQuery} disabled={false} />,
  );
  fireEvent.click(screen.getByRole("button", { name: "按交易日检查" }));
  view.rerender(
    <MinuteEvidencePanel key="service-b" id="minute" query={nextQuery} disabled={false} />,
  );
  fireEvent.click(screen.getByRole("button", { name: "按交易日检查" }));
  await screen.findByText("没有可归属到交易日的分钟记录。");
  await act(async () => release(reply({ ...evidence, days: evidence.days.slice(0, 50) })));
  expect(screen.queryByRole("table")).toBeNull();
});
