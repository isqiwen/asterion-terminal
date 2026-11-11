import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { Table } from "../../src/generated/asterion/warehouse_pb";
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

test("historical price ticks are browsed in the chosen fixed version and date range", async () => {
  render(
    <ReferenceTable
      version="historical-version"
      table={Table.PRICE_TICKS}
      name="历史报价单位"
      exchanges={["SHFE"]}
    />,
  );
  await screen.findByRole("table", { name: "历史报价单位" });
  fireEvent.change(screen.getByLabelText("开始日期"), { target: { value: "2014-01-01" } });
  fireEvent.change(screen.getByLabelText("结束日期"), { target: { value: "2014-12-31" } });
  await waitFor(() =>
    expect(request).toHaveBeenLastCalledWith({
      case: "getTableRows",
      value: {
        dataset: { version: "historical-version" },
        table: Table.PRICE_TICKS,
        exchange: "SHFE",
        firstDay: "2014-01-01",
        lastDay: "2014-12-31",
        offset: 0,
      },
    }),
  );
});
