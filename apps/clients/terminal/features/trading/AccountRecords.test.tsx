import { create } from "@bufbuild/protobuf";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { AccountRecords } from "./AccountRecords";
import { feature } from "./feature";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { Offset, Side } from "../../src/generated/asterion/common_pb";
import {
  AccountSchema,
  ResponseSchema,
  type Response,
} from "../../src/generated/asterion/trading_pb";
import { tradingRequest } from "../../src/bridge/trading";

vi.mock("../../src/bridge/trading", () => ({ tradingRequest: vi.fn() }));
registerTerminalFeatures([feature]);
test("changing a historical trading day cannot leave the previous day's fills exportable", async () => {
  const days = ["2026-11-12", "2026-11-13"];
  vi.mocked(tradingRequest).mockImplementation(async (_connection, operation) => {
    if (operation.case !== "getDay") throw new Error("Unexpected request");
    if (operation.value.tradingDay === "2026-11-13") throw new Error("Trading disconnected");
    return create(ResponseSchema, {
      result: {
        case: "getDay",
        value: {
          tradingDays: days,
          trades: operation.value.tradingDay
            ? [
                {
                  instrument: { exchange: "SHFE", symbol: "rb2701" },
                  tradeTime: "11-12 09:01:00",
                  price: { text: "3000" },
                  lots: { text: "1" },
                },
              ]
            : [],
        },
      },
    });
  });
  render(
    <AccountRecords
      connection="sim"
      record={create(AccountSchema, { session: { tradingDay: "2026-11-14" } })}
      busy={false}
      run={vi.fn()}
      activity="fills"
      setActivity={vi.fn()}
      ready={true}
      stale={false}
      cancelling={[]}
      cancel={vi.fn()}
      close={vi.fn()}
      separate={() => false}
    />,
  );
  const day = await screen.findByRole("combobox", { name: "交易日" });
  fireEvent.change(day, { target: { value: "2026-11-12" } });
  expect(await screen.findByText("11-12 09:01:00")).toBeTruthy();
  fireEvent.change(day, { target: { value: "2026-11-13" } });
  await waitFor(() =>
    expect(tradingRequest).toHaveBeenLastCalledWith("sim", {
      case: "getDay",
      value: { tradingDay: "2026-11-13", offset: 0, limit: 50 },
    }),
  );
  expect(screen.queryByText("11-12 09:01:00")).toBeNull();
  expect(screen.queryByRole("button", { name: "导出 CSV" })).toBeNull();
});

test("a paged historical day still exports all fills", async () => {
  const trades = Array.from({ length: 201 }, (_, index) => ({
    id: String(index),
    instrument: { exchange: "SHFE", symbol: "rb2701" },
    tradeTime: `fill-${index}`,
    price: { text: "3000" },
    lots: { text: "1" },
  }));
  vi.mocked(tradingRequest).mockReset();
  vi.mocked(tradingRequest).mockImplementation(async (_connection, operation) => {
    if (operation.case !== "getDay") throw new Error("Unexpected request");
    const { offset = 0, limit = 50, tradingDay } = operation.value;
    return create(ResponseSchema, {
      result: {
        case: "getDay",
        value: {
          tradingDays: ["2026-11-12"],
          total: tradingDay ? trades.length : 0,
          trades: tradingDay ? trades.slice(offset, offset + limit) : [],
        },
      },
    });
  });
  render(
    <AccountRecords
      connection="sim"
      record={create(AccountSchema, {
        session: { tradingDay: "2026-11-14" },
      })}
      busy={false}
      run={vi.fn()}
      activity="fills"
      setActivity={vi.fn()}
      ready
      stale={false}
      cancelling={[]}
      cancel={vi.fn()}
      close={vi.fn()}
      separate={() => false}
    />,
  );
  fireEvent.change(await screen.findByRole("combobox", { name: "交易日" }), {
    target: { value: "2026-11-12" },
  });
  await screen.findByText("fill-0");
  fireEvent.click(screen.getByRole("button", { name: "下一页" }));
  await screen.findByText("fill-50");
  expect(screen.queryByText("fill-0")).toBeNull();
  let exported: Blob | undefined;
  URL.createObjectURL = vi.fn((value: Blob) => {
    exported = value;
    return "blob:day";
  });
  URL.revokeObjectURL = vi.fn();
  const click = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => {});
  try {
    fireEvent.click(screen.getByRole("button", { name: "导出 CSV" }));
    await waitFor(() => expect(click).toHaveBeenCalledOnce());
    const csv = await exported!.text();
    expect(csv.match(/fill-\d+,/g)).toHaveLength(201);
    expect(csv).toContain("fill-200");
  } finally {
    click.mockRestore();
  }
});

test("order history pages keep working orders cancellable", async () => {
  const orders = Array.from({ length: 51 }, (_, index) => ({
    id: String(index),
    brokerKey: String(index),
    instrument: { exchange: "SHFE", symbol: `rb${index}` },
    state: index === 50 ? 2 : 4,
    lots: { text: "1" },
    price: { text: "3000" },
  }));
  vi.mocked(tradingRequest).mockReset();
  vi.mocked(tradingRequest).mockImplementation(async (_connection, operation) => {
    if (operation.case !== "getOrders") throw new Error("Unexpected request");
    const { offset = 0, limit = 50 } = operation.value;
    return create(ResponseSchema, {
      result: {
        case: "getOrders",
        value: {
          total: 51,
          orders: orders.slice(offset, offset + limit),
        },
      },
    });
  });
  const cancel = vi.fn(async () => {});
  render(
    <AccountRecords
      connection="sim"
      record={create(AccountSchema, {
        session: { orderCount: 51, workingOrders: [orders[50]!] },
      })}
      busy={false}
      run={vi.fn()}
      activity="orders"
      setActivity={vi.fn()}
      ready
      stale={false}
      cancelling={[]}
      cancel={cancel}
      close={vi.fn()}
      separate={() => false}
    />,
  );
  await screen.findByText("rb0");
  fireEvent.click(screen.getByRole("button", { name: "下一页" }));
  await screen.findByText("rb50");
  expect(screen.queryByText("rb0")).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "撤单" }));
  expect(cancel).toHaveBeenCalledExactlyOnceWith("50");
});

test.each(["orders", "fills"] as const)(
  "slow %s pages survive account status refreshes",
  async activity => {
    let deliver!: (reply: Response) => void;
    vi.mocked(tradingRequest).mockReset();
    vi.mocked(tradingRequest).mockImplementation(
      () =>
        new Promise(resolve => {
          deliver = resolve;
        }),
    );
    const panel = (tradeCount: number) => (
      <AccountRecords
        connection="sim"
        record={create(AccountSchema, {
          session: { tradingDay: "2026-11-14", orderCount: 1, tradeCount },
        })}
        busy={false}
        run={vi.fn()}
        activity={activity}
        setActivity={vi.fn()}
        ready
        stale={false}
        cancelling={[]}
        cancel={vi.fn()}
        close={vi.fn()}
        separate={() => false}
      />
    );
    const view = render(panel(1));
    view.rerender(panel(2));
    expect(tradingRequest).toHaveBeenCalledOnce();
    const row = { instrument: { exchange: "SHFE", symbol: "rb2701" } };
    deliver(
      create(ResponseSchema, {
        result:
          activity === "orders"
            ? { case: "getOrders", value: { total: 1, orders: [row] } }
            : { case: "getDay", value: { total: 1, trades: [row] } },
      }),
    );
    expect(await screen.findByText("rb2701")).toBeTruthy();
  },
);

test("today's export keeps its fills and account snapshot when new fills arrive and the counter rolls over", async () => {
  vi.mocked(tradingRequest).mockReset();
  const date = "2026-11-14";
  let today = date;
  const trades = Array.from({ length: 201 }, (_, index) => ({
    instrument: { exchange: "SHFE", symbol: "rb2701" },
    tradeTime: `fill-${index}`,
    price: { text: "3210" },
    lots: { text: "1" },
  }));
  vi.mocked(tradingRequest).mockImplementation(async (_connection, operation) => {
    if (operation.case !== "getDay") throw new Error("Unexpected request");
    const { tradingDay, offset = 0, limit = 50, oldestFirst } = operation.value;
    const rows = (tradingDay || today) === date ? trades : [];
    const response = create(ResponseSchema, {
      result: {
        case: "getDay",
        value: {
          tradingDays: ["2026-11-14"],
          total: rows.length,
          trades: (oldestFirst ? rows : [...rows].reverse()).slice(offset, offset + limit),
          close: {
            tradingDay: "2026-11-14",
            funds: {
              balance: { text: today === date ? "100000" : "120000" },
              available: { text: "90000" },
              margin: { text: "10000" },
              commission: { text: "12" },
              positionProfit: { text: "50" },
              closeProfit: { text: "0" },
            },
            positions: [
              {
                instrument: { exchange: "SHFE", symbol: "rb2701" },
                side: 1,
                todayLots: { text: "1" },
                yesterdayLots: { text: "0" },
                cost: { text: "32100" },
                profit: { text: "50" },
                margin: { text: "3210" },
              },
            ],
            cancels: [
              {
                instrument: { exchange: "SHFE", symbol: "rb2701" },
                cancels: 3,
                rejectedCancels: 1,
              },
            ],
          },
        },
      },
    });
    if (oldestFirst && !offset) {
      trades.push({ ...trades[0]!, tradeTime: "later-fill" });
      today = "2026-11-16";
    }
    return response;
  });
  render(
    <AccountRecords
      connection="sim"
      record={create(AccountSchema, { session: { tradingDay: "2026-11-14" } })}
      busy={false}
      run={vi.fn()}
      activity="fills"
      setActivity={vi.fn()}
      ready
      stale={false}
      cancelling={[]}
      cancel={vi.fn()}
      close={vi.fn()}
      separate={() => false}
    />,
  );
  expect(await screen.findByRole("table", { name: "撤单统计" })).toBeTruthy();
  let exported: Blob | undefined;
  let filename = "";
  URL.createObjectURL = vi.fn((value: Blob) => {
    exported = value;
    return "blob:today";
  });
  URL.revokeObjectURL = vi.fn();
  const click = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(function (
    this: HTMLAnchorElement,
  ) {
    filename = this.download;
  });
  try {
    fireEvent.click(screen.getByRole("button", { name: "导出 CSV" }));
    await waitFor(() => expect(click).toHaveBeenCalledOnce());
    expect(filename).toBe("sim-2026-11-14.csv");
    const csv = await exported!.text();
    expect(csv).toContain("2026-11-14");
    expect(csv).toContain("100,000");
    expect(csv).toContain("rb2701,3,1");
    expect(csv.match(/fill-\d+,/g)).toEqual(
      Array.from({ length: 201 }, (_, i) => `fill-${200 - i},`),
    );
    expect(csv).not.toContain("later-fill");
  } finally {
    click.mockRestore();
  }
});

test("product-whitelisted positions compute average price and unclosable rows ignore double-click", () => {
  const close = vi.fn();
  render(
    <AccountRecords
      connection="sim"
      record={create(AccountSchema, {
        session: {
          policy: {
            contracts: [
              {
                instrument: { exchange: "SHFE", symbol: "rb" },
                product: "rb",
                deliveryMonth: "",
                multiplier: { text: "10" },
                priceTick: { text: "1" },
                lotSize: { text: "1" },
              },
            ],
          },
          positions: [
            {
              instrument: { exchange: "SHFE", symbol: "rb2701" },
              side: Side.BUY,
              todayLots: { text: "2" },
              yesterdayLots: { text: "0" },
              cost: { text: "70000" },
              profit: { text: "100" },
              margin: { text: "7000" },
            },
          ],
          workingOrders: [
            {
              id: "close-all",
              instrument: { exchange: "SHFE", symbol: "rb2701" },
              side: Side.SELL,
              offset: Offset.CLOSE_TODAY,
              lots: { text: "2" },
              filledLots: { text: "0" },
              state: 2,
            },
          ],
        },
      })}
      busy={false}
      run={vi.fn()}
      activity="positions"
      setActivity={vi.fn()}
      ready
      stale={false}
      cancelling={[]}
      cancel={vi.fn()}
      close={close}
      separate={() => true}
    />,
  );
  const row = screen.getByRole("cell", { name: "3500.00" }).closest("tr")!;
  fireEvent.doubleClick(row);
  expect(close).not.toHaveBeenCalled();
});

test("clicking a position row selects its contract and renders a totals footer", () => {
  const onSelectContract = vi.fn();
  render(
    <AccountRecords
      connection="sim"
      record={create(AccountSchema, {
        session: {
          policy: {
            contracts: [
              {
                instrument: { exchange: "SHFE", symbol: "rb2701" },
                product: "rb",
                deliveryMonth: "2027-01",
                multiplier: { text: "10" },
                priceTick: { text: "1" },
                lotSize: { text: "1" },
              },
            ],
          },
          positions: [
            {
              instrument: { exchange: "SHFE", symbol: "rb2701" },
              side: Side.BUY,
              todayLots: { text: "2" },
              yesterdayLots: { text: "1" },
              cost: { text: "105000" },
              profit: { text: "600" },
              margin: { text: "10000" },
            },
          ],
        },
      })}
      busy={false}
      run={vi.fn()}
      activity="positions"
      setActivity={vi.fn()}
      ready
      stale={false}
      cancelling={[]}
      cancel={vi.fn()}
      close={vi.fn()}
      separate={() => true}
      selectedContract="SHFE.rb2701"
      onSelectContract={onSelectContract}
    />,
  );
  const row = screen.getByRole("cell", { name: "3500.00" }).closest("tr")!;
  expect(row.dataset.selected).toBe("true");
  fireEvent.click(row);
  expect(onSelectContract).toHaveBeenCalledWith("SHFE.rb2701");
  expect(screen.getByText("合计 (1)")).toBeTruthy();
  expect(screen.getAllByText("+6.0%").length).toBeGreaterThanOrEqual(1);
});
