import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { create } from "@bufbuild/protobuf";
import { Table, TableRowsSchema } from "../../src/generated/asterion/warehouse_pb";
import { ReferenceTable } from "./ReferenceTable";

const request = vi.hoisted(() => vi.fn());
vi.mock("../../src/bridge/warehouse-data", () => ({
  warehouseData: request,
}));
vi.mock("../../src/i18n", async importOriginal => ({
  ...(await importOriginal<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));
request.mockImplementation(async (body: { value: { exchange: string } }) => ({
  columns: ["exchange", "contract"],
  rows: [{ values: [body.value.exchange, `${body.value.exchange}/x/2027-01`] }],
  total: 1,
}));

test("the contract table shows the chosen exchange's contracts", async () => {
  render(
    <ReferenceTable
      version="v"
      table={Table.CONTRACTS}
      name="合约表"
      exchanges={["SHFE", "DCE"]}
    />,
  );
  await screen.findByRole("cell", { name: "SHFE/x/2027-01" });
  expect(screen.getByRole("columnheader", { name: "交易所" })).toBeTruthy();
  expect(screen.getByRole("columnheader", { name: "合约" })).toBeTruthy();
  fireEvent.change(screen.getByLabelText("交易所"), { target: { value: "DCE" } });
  await screen.findByRole("cell", { name: "DCE/x/2027-01" });
  await waitFor(() =>
    expect(request).toHaveBeenLastCalledWith({
      case: "getTableRows",
      value: expect.objectContaining({
        dataset: { version: "v" },
        table: Table.CONTRACTS,
        exchange: "DCE",
        offset: 0,
      }),
    }),
  );
  expect(screen.getByText("第 1–1 行，共 1 行")).toBeTruthy();
});

test("historical price ticks are browsed in the chosen fixed version", async () => {
  render(
    <ReferenceTable
      version="historical-version"
      table={Table.PRICE_TICKS}
      name="历史报价单位"
      exchanges={["SHFE"]}
    />,
  );
  await screen.findByRole("table", { name: "历史报价单位" });
  await waitFor(() =>
    expect(request).toHaveBeenLastCalledWith({
      case: "getTableRows",
      value: {
        dataset: { version: "historical-version" },
        table: Table.PRICE_TICKS,
        exchange: "SHFE",
        offset: 0,
      },
    }),
  );
});

test("failed exchange change does not present the previous exchange's rows as the new one", async () => {
  request.mockReset();
  request.mockResolvedValueOnce(
    create(TableRowsSchema, {
      columns: ["contract"],
      rows: [{ values: ["SHFE/rb/2027-01"] }],
      total: 1,
    }) as never,
  );
  request.mockRejectedValueOnce(new Error("Warehouse disconnected"));
  render(
    <ReferenceTable
      version="v1"
      table={Table.CONTRACTS}
      name="合约表"
      exchanges={["SHFE", "DCE"]}
    />,
  );
  expect(await screen.findByText("SHFE/rb/2027-01")).toBeTruthy();
  fireEvent.change(screen.getByRole("combobox", { name: "交易所" }), { target: { value: "DCE" } });
  await waitFor(() => expect(request).toHaveBeenCalledTimes(2));
  expect(screen.queryByText("SHFE/rb/2027-01")).toBeNull();
});

test("inventory shows the report date and unknown release time, not when it was collected", async () => {
  request.mockReset();
  request.mockResolvedValue({
    columns: ["day", "unit", "total", "published_ms", "observed_ms"],
    rows: [{ values: ["2026-11-20", "吨", "353.123456", "", "1795248000000"] }],
    total: 1,
  });
  render(
    <ReferenceTable version="v" table={Table.INVENTORY} name="库存周报" exchanges={["SHFE"]} />,
  );
  await screen.findByRole("table", { name: "库存周报" });
  expect(screen.getByRole("columnheader", { name: "报告日期" })).toBeTruthy();
  expect(screen.getByRole("columnheader", { name: "发布时间（北京时间）" })).toBeTruthy();
  expect(screen.getByRole("cell", { name: "353.123456" })).toBeTruthy();
  expect(screen.getByRole("cell", { name: "—" })).toBeTruthy();
  expect(screen.getAllByRole("columnheader")).toHaveLength(4);
  expect(screen.queryByRole("cell", { name: "2026-11-21 16:00:00" })).toBeNull();
});
