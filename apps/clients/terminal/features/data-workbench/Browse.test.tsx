import { act, fireEvent, render, screen, within } from "@testing-library/react";
import { afterEach, beforeEach, expect, test, vi } from "vitest";
import { Table } from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { Browse } from "./Browse";

const request = vi.hoisted(() => vi.fn());
afterEach(() => vi.useRealTimers());
vi.mock("../../src/bridge/warehouse-data", () => ({ warehouseData: request }));
vi.mock("../../src/i18n", async importOriginal => ({
  ...(await importOriginal<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));

beforeEach(() => {
  request.mockReset();
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables: [{ table: Table.INVENTORY, exchanges: ["SHFE", "INE"] }] };
      case "getDatasetSummary":
        return { dataset: { version: "current" } };
      case "getDatasetCoverage":
        return { products: [], contracts: [] };
      case "getTableRows":
        return { columns: ["warehouse"], rows: [{ values: ["总计"] }], total: 1 };
      case "compareDatasetVersions":
        return { added: 1, replaced: 0, removed: 0 };
      default:
        return {};
    }
  });
});

const view = (version = "") => {
  const context = {
    navigate: vi.fn(),
    snapshot: { warehouse_endpoint: { host: "warehouse", port: 7450, fingerprint: "f" } },
    workspaceParams: { table: String(Table.INVENTORY), version },
  } as unknown as TerminalContext;
  return render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.data-browse">
      <Browse {...context} />
    </WorkspaceDraftScope>,
  );
};

test("opening an earlier report version keeps its reads fixed", async () => {
  vi.useFakeTimers();
  await act(async () => {
    view("earlier");
  });
  await act(async () => {
    await vi.advanceTimersByTimeAsync(6000);
  });
  expect(screen.getByRole("table", { name: "库存周报" })).toBeTruthy();
  expect(request).toHaveBeenCalledWith({
    case: "getTableRows",
    value: expect.objectContaining({ table: Table.INVENTORY, dataset: { version: "earlier" } }),
  });
  expect(request.mock.calls.filter(([body]) => body.case === "getDatasetSummary")).toHaveLength(1);
});

test("new reports appear without reopening the browser or losing its filters", async () => {
  vi.useFakeTimers();
  let latest = "before";
  request.mockImplementation(async (body: { case: string; value: { offset?: number } }) => {
    switch (body.case) {
      case "getDatasetSummary":
        return { dataset: { version: latest } };
      case "getDatasetCoverage":
        return { products: [] };
      case "getTableRows":
        return {
          columns: ["warehouse"],
          rows: [{ values: [latest === "after" ? "新报告" : `原报告 ${body.value.offset}`] }],
          total: latest === "after" ? 1 : 101,
        };
      default:
        return {};
    }
  });
  await act(async () => {
    view();
  });
  await act(async () => {
    fireEvent.change(screen.getByLabelText("交易所"), { target: { value: "INE" } });
    fireEvent.change(screen.getByLabelText("开始日期"), { target: { value: "2018-11-13" } });
    fireEvent.change(screen.getByLabelText("结束日期"), { target: { value: "2018-11-16" } });
  });
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name: "下一页" }));
  });
  expect(screen.getByText("原报告 100")).toBeTruthy();
  latest = "after";
  await act(async () => {
    await vi.advanceTimersByTimeAsync(3000);
  });
  expect(screen.getByText("新报告")).toBeTruthy();
  expect(request).toHaveBeenLastCalledWith({
    case: "getTableRows",
    value: {
      dataset: { version: "after" },
      table: Table.INVENTORY,
      exchange: "INE",
      firstDay: "2018-11-13",
      lastDay: "2018-11-16",
      offset: 0,
    },
  });
  expect((screen.getByLabelText("交易所") as HTMLSelectElement).value).toBe("INE");
  expect((screen.getByLabelText("开始日期") as HTMLInputElement).value).toBe("2018-11-13");
  expect((screen.getByLabelText("结束日期") as HTMLInputElement).value).toBe("2018-11-16");
});

test("tables are grouped by what they are for, and a table opened by link shows its group", async () => {
  view();
  await screen.findByRole("table", { name: "库存周报" });
  const categories = within(screen.getByRole("group", { name: "数据分类" }));
  expect(categories.getAllByRole("button").map(button => button.textContent)).toEqual([
    "行情",
    "基本面",
    "交易参数",
  ]);
  expect(categories.getByRole("button", { name: "基本面" }).getAttribute("aria-pressed")).toBe(
    "true",
  );
  expect(
    within(screen.getByRole("group", { name: "基本面" }))
      .getAllByRole("button")
      .map(button => button.textContent),
  ).toEqual(["仓单", "会员持仓", "库存周报"]);

  // Pressing the open group keeps the table; another group opens on its first.
  fireEvent.click(categories.getByRole("button", { name: "基本面" }));
  expect(screen.getByRole("table", { name: "库存周报" })).toBeTruthy();
  fireEvent.click(categories.getByRole("button", { name: "交易参数" }));
  expect(await screen.findByRole("table", { name: "结算参数" })).toBeTruthy();
  fireEvent.click(categories.getByRole("button", { name: "行情" }));
  expect(screen.queryByRole("group", { name: "行情" })).toBeNull();
});
