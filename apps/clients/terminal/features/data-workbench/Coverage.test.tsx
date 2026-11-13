import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { afterEach, expect, test, vi } from "vitest";
import { create } from "@bufbuild/protobuf";
import { DatasetCoverageSchema, Table } from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { Coverage } from "./Coverage";

const request = vi.hoisted(() => vi.fn());
vi.mock("../../src/bridge/warehouse-data", () => ({
  warehouseData: request,
}));
vi.mock("../../src/i18n", async importOriginal => ({
  ...(await importOriginal<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));
// jsdom does not implement the browser's dialog methods.
HTMLDialogElement.prototype.showModal = function () {
  this.setAttribute("open", "");
};
HTMLDialogElement.prototype.close = function () {
  this.removeAttribute("open");
};
afterEach(() => vi.restoreAllMocks());
const context = (params?: Record<string, string>) =>
  ({
    trade: vi.fn(),
    snapshot: { warehouse_endpoint: { host: "market", port: 7450, fingerprint: "f" } },
    workspaceParams: params,
  }) as unknown as TerminalContext;
const view = (props: TerminalContext) =>
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.data-coverage">
      <Coverage {...props} />
    </WorkspaceDraftScope>,
  );
const older = "a".repeat(64);
const current = "b".repeat(64);

// Daily and minute bars on offer, and what each version holds: the current one
// rebar with a missing August and a partial September, the older one copper.
function serve(tables = [Table.DAILY, Table.MINUTES, Table.PRICE_TICKS]) {
  request.mockImplementation(async (body: { case: string; value: { version?: string } }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables, exchanges: ["SHFE"] };
      case "getDatasetSummary":
        return { dataset: { version: current }, tables: [] };
      case "getDatasetCoverage":
        return create(
          DatasetCoverageSchema,
          body.value.version === older
            ? {
                products: [
                  { product: "SHFE/cu", months: [{ month: "2026-10", days: 21, openDays: 21 }] },
                ],
              }
            : {
                products: [
                  {
                    product: "SHFE/rb",
                    months: [
                      { month: "2026-07", days: 23, openDays: 23 },
                      { month: "2026-09", days: 10, openDays: 21 },
                      { month: "2026-10", days: 16, openDays: 16 },
                    ],
                  },
                  // Every source answered the other six days empty.
                  {
                    product: "SHFE/au",
                    months: [{ month: "2026-10", days: 10, openDays: 16, emptyDays: 6 }],
                  },
                  // The last four days have yet to come with an update.
                  {
                    product: "SHFE/ag",
                    months: [{ month: "2026-10", days: 12, openDays: 16, pendingDays: 4 }],
                  },
                ],
                contracts: [
                  {
                    contract: "SHFE/cu/2026-12",
                    months: [{ month: "2026-10", days: 3, openDays: 16 }],
                  },
                  {
                    contract: "SHFE/rb/2027-01",
                    months: [{ month: "2026-10", days: 3, openDays: 16 }],
                  },
                ],
              },
        );
      case "compareDatasetVersions":
        return { added: 1, replaced: 0, removed: 0 };
      default:
        return {};
    }
  });
}
const submitted = () =>
  request.mock.calls
    .map(([body]) => body)
    .filter(body => body.case === "startAcquisition")
    .map(body => body.value);
const startInDialog = () =>
  fireEvent.click(within(screen.getByRole("dialog")).getByRole("button", { name: "开始采集" }));
const show = (name: string) =>
  fireEvent.click(
    within(screen.getByRole("group", { name: "数据" })).getByRole("button", { name }),
  );

test("each product's months show complete, partial and missing against the calendar", async () => {
  serve();
  view(context());
  const matrix = await screen.findByRole("table", { name: "日线覆盖" });
  expect(request).toHaveBeenCalledWith({ case: "getDatasetCoverage", value: { version: current } });
  const row = within(matrix).getByRole("row", { name: /SHFE\/rb/ });
  const cells = within(row).getAllByRole("cell");
  expect(cells.slice(6, 10).map(cell => cell.getAttribute("data-cell"))).toEqual([
    "full",
    "missing",
    "partial",
    "full",
  ]);
  // Months before the first and after the last held one are not gaps.
  expect(cells[5]!.hasAttribute("data-cell")).toBe(false);
  expect(cells[10]!.hasAttribute("data-cell")).toBe(false);
  // Days no source has are not offered again.
  const gold = within(within(matrix).getByRole("row", { name: /SHFE\/au/ })).getAllByRole("cell");
  expect(gold[9]!.getAttribute("data-cell")).toBe("empty");
  expect(screen.queryByRole("button", { name: "补采 SHFE/au 2026-10" })).toBeNull();
  // Days only an update brings are told apart from gaps.
  const silver = within(within(matrix).getByRole("row", { name: /SHFE\/ag/ })).getAllByRole("cell");
  expect(silver[9]!.getAttribute("data-cell")).toBe("pending");
  expect(silver[9]!.getAttribute("title")).toBe("12 / 16 · 4 天待更新");
});

test("a gap opens an acquisition of its product or contract and month only", async () => {
  serve();
  view(context());
  fireEvent.click(await screen.findByRole("button", { name: "补采 SHFE/rb 2026-08" }));
  const dialog = screen.getByRole("dialog");
  expect((within(dialog).getByLabelText("品种或合约") as HTMLInputElement).checked).toBe(true);
  startInDialog();
  await waitFor(() =>
    expect(submitted()[0]).toMatchObject({
      table: Table.DAILY,
      exchanges: [],
      instruments: ["SHFE/rb"],
      firstDay: "2026-08-01",
      lastDay: "2026-08-31",
    }),
  );
  await waitFor(() => expect(screen.queryByRole("dialog")).toBeNull());
  request.mockClear();
  // Minutes are measured against their own open days.
  show("分钟线");
  fireEvent.click(await screen.findByRole("button", { name: "补采 SHFE/rb/2027-01 2026-10" }));
  startInDialog();
  await waitFor(() =>
    expect(submitted()[0]).toMatchObject({
      table: Table.MINUTES,
      instruments: ["SHFE/rb/2027-01"],
      firstDay: "2026-10-01",
      lastDay: "2026-10-31",
    }),
  );
});

test("clients choose no source, and daily bars also ask for their price ticks", async () => {
  serve();
  const props = context();
  view(props);
  fireEvent.click(await screen.findByRole("button", { name: "采集" }));
  const dialog = screen.getByRole("dialog", { name: /^采集 / });
  expect(within(dialog).queryByLabelText("数据源")).toBeNull();
  startInDialog();
  await waitFor(() =>
    expect(submitted().map(value => [value.table, value.exchanges])).toEqual([
      [Table.DAILY, ["SHFE"]],
      [Table.PRICE_TICKS, ["SHFE"]],
    ]),
  );
  await waitFor(() => expect(screen.queryByRole("dialog")).toBeNull());
  expect(request.mock.calls.flat().some(body => "provider" in (body.value ?? {}))).toBe(false);
  expect(props.trade).not.toHaveBeenCalled();
});

test("a minute acquisition of a whole product spans its contracts' listed lives", async () => {
  serve();
  view(context());
  await screen.findByRole("table", { name: "日线覆盖" });
  show("分钟线");
  fireEvent.click(await screen.findByRole("button", { name: "采集" }));
  const picker = within(screen.getByRole("dialog")).getByRole("group", { name: "品种或合约" });
  fireEvent.change(within(picker).getByLabelText("品种"), { target: { value: "SHFE/rb" } });
  fireEvent.click(within(picker).getByRole("button", { name: "整个品种" }));
  startInDialog();
  await waitFor(() =>
    expect(submitted()[0]).toMatchObject({
      table: Table.MINUTES,
      exchanges: [],
      instruments: ["SHFE/rb"],
      firstDay: "",
      lastDay: "",
    }),
  );
});

test("daily bars of one contract ask for it and its price ticks, not its exchange", async () => {
  serve();
  view(context());
  fireEvent.click(await screen.findByRole("button", { name: "采集" }));
  const dialog = screen.getByRole("dialog");
  fireEvent.click(within(dialog).getByRole("radio", { name: "品种或合约" }));
  const picker = within(dialog).getByRole("group", { name: "品种或合约" });
  fireEvent.change(within(picker).getByLabelText("其他品种或合约"), {
    target: { value: "SHFE/rb/2027-01" },
  });
  fireEvent.click(within(picker).getByRole("button", { name: "添加" }));
  startInDialog();
  await waitFor(() => expect(submitted()).toHaveLength(2));
  for (const value of submitted())
    expect(value).toMatchObject({
      exchanges: [],
      instruments: ["SHFE/rb/2027-01"],
      firstDay: "",
      lastDay: "",
    });
});

test("without a source for the table nothing offers an acquisition", async () => {
  serve([]);
  view(context());
  await screen.findByRole("rowheader", { name: "SHFE/rb" });
  expect(screen.queryByRole("button", { name: "采集" })).toBeNull();
  expect(screen.queryByRole("button", { name: /^补采/ })).toBeNull();
});

test("a research run's version shows its own coverage until the latest is chosen", async () => {
  serve();
  view(context({ version: older }));
  await screen.findByRole("rowheader", { name: "SHFE/cu" });
  expect(screen.queryByRole("button", { name: "采集" })).toBeNull();
  await waitFor(() =>
    expect(screen.getByRole("status").textContent).toContain("新增 1 · 替换 0 · 删除 0"),
  );
  fireEvent.click(screen.getByRole("button", { name: "回到最新数据" }));
  await screen.findByRole("rowheader", { name: "SHFE/rb" });
});

test("the view says it is reading while the data service has not answered", async () => {
  request.mockImplementation(() => new Promise(() => {}));
  view(context());
  expect((await screen.findByRole("status")).textContent).toBe("读取中");
});
