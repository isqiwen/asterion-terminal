import { create } from "@bufbuild/protobuf";
import { render, screen, waitFor, within } from "@testing-library/react";
import { beforeEach, expect, test, vi } from "vitest";
import { tradingRequest } from "./bridge/trading";
import { warehouseData } from "./bridge/warehouse-data";
import { AccountSchema, Environment, ResponseSchema } from "./generated/asterion/trading_pb";
import { AcquisitionTasksSchema, StatusSchema } from "./generated/asterion/warehouse_pb";
import { modules } from "./host/navigation";
import { TradingAccountsProvider } from "./host/trading-accounts";
import { WarehouseProvider } from "./host/warehouse";
import { ModuleHeader } from "./App";

vi.mock("./bridge/warehouse-data", () => ({ warehouseData: vi.fn() }));
vi.mock("./bridge/trading", () => ({ tradingRequest: vi.fn() }));

beforeEach(() => {
  vi.mocked(warehouseData).mockReset();
  vi.mocked(tradingRequest).mockReset();
});

const moduleById = (id: string) => modules.find(item => item.id === id)!;

test("data subtabs stay hidden until Warehouse connects", async () => {
  vi.mocked(warehouseData).mockRejectedValue(new Error("warehouse offline"));
  const { rerender } = render(
    <WarehouseProvider endpoint="">
      <ModuleHeader
        module={moduleById("data")}
        view="workspace.data-dataset"
        warehouseConfigured={false}
        tradingConnections={[]}
        navigate={vi.fn()}
      />
    </WarehouseProvider>,
  );
  expect(screen.queryByRole("navigation", { name: "子模块视图" })).toBeNull();

  rerender(
    <WarehouseProvider endpoint="warehouse:7450/f1">
      <ModuleHeader
        module={moduleById("data")}
        view="workspace.data-dataset"
        warehouseConfigured={true}
        tradingConnections={[]}
        navigate={vi.fn()}
      />
    </WarehouseProvider>,
  );
  await waitFor(() => expect(warehouseData).toHaveBeenCalled());
  expect(screen.queryByRole("navigation", { name: "子模块视图" })).toBeNull();

  vi.mocked(warehouseData).mockImplementation(async operation =>
    operation.case === "listAcquisitionTasks"
      ? create(AcquisitionTasksSchema, { tasks: [], nextId: "" })
      : create(StatusSchema, {}),
  );
  rerender(
    <WarehouseProvider endpoint="warehouse:7450/f2">
      <ModuleHeader
        module={moduleById("data")}
        view="workspace.data-dataset"
        warehouseConfigured={true}
        tradingConnections={[]}
        navigate={vi.fn()}
      />
    </WarehouseProvider>,
  );
  const nav = await screen.findByRole("navigation", { name: "子模块视图" });
  expect(
    within(nav)
      .getAllByRole("button")
      .map(item => item.textContent),
  ).toEqual(["采集", "浏览"]);
});

test("trading subtabs hide when unconfigured or unreachable and appear once an account answers", async () => {
  vi.mocked(tradingRequest).mockRejectedValue(new Error("connection refused"));
  const { rerender } = render(
    <TradingAccountsProvider connections={[]}>
      <ModuleHeader
        module={moduleById("live")}
        view="workspace.trading"
        warehouseConfigured={false}
        tradingConnections={[]}
        navigate={vi.fn()}
      />
    </TradingAccountsProvider>,
  );
  expect(screen.queryByRole("navigation", { name: "子模块视图" })).toBeNull();

  rerender(
    <TradingAccountsProvider connections={[{ id: "acct", revision: "1" }]}>
      <ModuleHeader
        module={moduleById("live")}
        view="workspace.trading"
        warehouseConfigured={false}
        tradingConnections={[{ id: "acct" }]}
        navigate={vi.fn()}
      />
    </TradingAccountsProvider>,
  );
  await waitFor(() => expect(tradingRequest).toHaveBeenCalled());
  expect(screen.queryByRole("navigation", { name: "子模块视图" })).toBeNull();

  vi.mocked(tradingRequest).mockImplementation(async (_account, operation) =>
    operation.case === "getAccount"
      ? create(ResponseSchema, {
          result: {
            case: "getAccount",
            value: create(AccountSchema, { id: "record", environment: Environment.SIMULATION }),
          },
        })
      : create(ResponseSchema, { result: { case: "getStrategies", value: {} } }),
  );
  rerender(
    <TradingAccountsProvider connections={[{ id: "acct", revision: "2" }]}>
      <ModuleHeader
        module={moduleById("live")}
        view="workspace.trading"
        warehouseConfigured={false}
        tradingConnections={[{ id: "acct" }]}
        navigate={vi.fn()}
      />
    </TradingAccountsProvider>,
  );
  const nav = await screen.findByRole("navigation", { name: "子模块视图" });
  expect(
    within(nav)
      .getAllByRole("button")
      .map(item => item.textContent),
  ).toEqual(["工作台", "策略"]);
});

test("research subtabs stay visible without remote services", () => {
  render(
    <ModuleHeader
      module={moduleById("research")}
      view="workspace.lab-strategies"
      warehouseConfigured={false}
      tradingConnections={[]}
      navigate={vi.fn()}
    />,
  );
  const nav = screen.getByRole("navigation", { name: "子模块视图" });
  expect(
    within(nav)
      .getAllByRole("button")
      .map(item => item.textContent),
  ).toEqual(["因子", "策略", "回测"]);
});
