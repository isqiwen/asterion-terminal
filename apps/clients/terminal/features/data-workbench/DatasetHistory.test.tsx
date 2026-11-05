import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { Table } from "../../src/generated/asterion/market_pb";
import type { TerminalContext } from "../contract";
import { Coverage } from "./Coverage";

const request = vi.hoisted(() => vi.fn());
vi.mock("../contract", async importOriginal => ({
  ...(await importOriginal<typeof import("../contract")>()),
  marketData: request,
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));
vi.mock("./MinuteComparison", () => ({ MinuteComparison: () => null }));
vi.mock("./RecordedDays", () => ({ RecordedDays: () => null }));

const older = "a".repeat(64);
const current = "b".repeat(64);
const context = (params?: Record<string, string>) =>
  ({
    trade: vi.fn(),
    snapshot: { market_endpoint: { host: "market", port: 7450, fingerprint: "f" } },
    workspaceParams: params,
  }) as unknown as TerminalContext;
// Research has two versions: the older one holds copper, the current one rebar.
request.mockImplementation(
  async (body: { case: string; value: { version?: string; afterVersion?: string } }) => {
    const version = body.value.version || current;
    switch (body.case) {
      case "listSources":
        return { sources: [], canManage: false };
      case "listDatasets":
        return { datasets: [{ provider: "research", version: current }] };
      case "listDatasetVersions":
        return {
          versions: [
            { dataset: { version: current, createdMs: 2n }, base: older, added: 1 },
            { dataset: { version: older, createdMs: 1n }, added: 2 },
          ],
          nextVersion: "",
        };
      case "getDatasetSummary":
        return {
          dataset: { version },
          tables: [{ table: Table.DAILY, rows: version === current ? 5n : 3n, partitions: 1n }],
        };
      case "getDatasetCoverage":
        return {
          products: [
            {
              product: version === current ? "SHFE/rb" : "SHFE/cu",
              months: [{ month: "2026-10", days: 1, openDays: 1 }],
            },
          ],
          contracts: [],
        };
      case "compareDatasetVersions":
        return { added: 1, replaced: 0, removed: 0 };
      case "listAcquisitionTasks":
        return { tasks: [], nextId: "" };
      default:
        return {};
    }
  },
);

test("viewing an earlier version shows its summary and coverage and what changed since", async () => {
  render(<Coverage {...context()} />);
  await screen.findByRole("rowheader", { name: "SHFE/rb" });
  const history = screen.getByRole("table", { name: "版本历史" });
  const row = within(history).getByRole("row", { name: new RegExp(older.slice(0, 12)) });
  fireEvent.click(within(row).getByRole("button", { name: "查看" }));
  await screen.findByRole("rowheader", { name: "SHFE/cu" });
  expect(screen.queryByRole("rowheader", { name: "SHFE/rb" })).toBeNull();
  expect(request).toHaveBeenCalledWith({
    case: "getDatasetSummary",
    value: { provider: "research", version: older },
  });
  await waitFor(() =>
    expect(screen.getByRole("status").textContent).toContain("新增 1 · 替换 0 · 删除 0"),
  );
  fireEvent.click(screen.getByRole("button", { name: "回到当前版本" }));
  await screen.findByRole("rowheader", { name: "SHFE/rb" });
});

test("a research run's version opens on the data page", async () => {
  render(<Coverage {...context({ provider: "research", version: older })} />);
  await screen.findByRole("rowheader", { name: "SHFE/cu" });
});
