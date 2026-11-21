import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { Table } from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { Browse } from "./Browse";

const request = vi.hoisted(() => vi.fn());
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

test("inventory acquisition offers only the exchanges supported by that table", async () => {
  view();
  await screen.findByRole("table", { name: "库存周报" });
  fireEvent.click(screen.getByRole("button", { name: "采集" }));
  const dialog = within(screen.getByRole("dialog"));
  expect(dialog.getAllByRole("checkbox")).toHaveLength(3);
  expect(dialog.getByRole("checkbox", { name: "SHFE" })).toBeTruthy();
  expect(dialog.getByRole("checkbox", { name: "INE" })).toBeTruthy();
  expect(dialog.queryByRole("checkbox", { name: "CFFEX" })).toBeNull();
  fireEvent.change(dialog.getByLabelText("开始日期"), { target: { value: "2026-01-05" } });
  fireEvent.change(dialog.getByLabelText("结束日期"), { target: { value: "2026-01-09" } });
  fireEvent.click(dialog.getByRole("button", { name: "开始采集" }));
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      case: "startAcquisition",
      value: expect.objectContaining({
        table: Table.INVENTORY,
        exchanges: ["SHFE"],
        firstDay: "2026-01-05",
        lastDay: "2026-01-09",
      }),
    }),
  );
  await waitFor(() => expect(screen.queryByRole("dialog")).toBeNull());
});

test("opening an earlier report version keeps its reads fixed and hides acquisition", async () => {
  view("earlier");
  await screen.findByRole("table", { name: "库存周报" });
  expect(screen.queryByRole("button", { name: "采集" })).toBeNull();
  expect(request).toHaveBeenCalledWith({
    case: "getTableRows",
    value: expect.objectContaining({ table: Table.INVENTORY, dataset: { version: "earlier" } }),
  });
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
