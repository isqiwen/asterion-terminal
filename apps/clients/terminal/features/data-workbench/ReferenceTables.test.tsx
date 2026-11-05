import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { Table } from "../../src/generated/asterion/market_pb";
import { ReferenceTables } from "./ReferenceTables";

const request = vi.hoisted(() => vi.fn());
vi.mock("../contract", async importOriginal => ({
  ...(await importOriginal<typeof import("../contract")>()),
  marketData: request,
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
    <ReferenceTables
      provider="tushare"
      version="v"
      tables={[Table.CONTRACTS, Table.CALENDAR]}
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
        dataset: { provider: "tushare", version: "v" },
        table: Table.CONTRACTS,
        exchange: "DCE",
        offset: 0,
      }),
    }),
  );
  expect(screen.getByText("第 1–1 行，共 1 行")).toBeTruthy();
});
