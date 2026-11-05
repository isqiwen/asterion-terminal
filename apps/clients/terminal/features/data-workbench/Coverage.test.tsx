import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { afterEach, expect, test, vi } from "vitest";
import { AcquisitionTaskSchema, Table, TaskState } from "../../src/generated/asterion/market_pb";
import type { TerminalContext } from "../contract";
import { Coverage } from "./Coverage";

const request = vi.hoisted(() => vi.fn());
vi.mock("../contract", async importOriginal => ({
  ...(await importOriginal<typeof import("../contract")>()),
  marketData: request,
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));
// The comparison has its own test.
vi.mock("./MinuteComparison", () => ({ MinuteComparison: () => null }));
vi.mock("./RecordedDays", () => ({ RecordedDays: () => null }));
vi.mock("./DatasetHistory", async importOriginal => ({
  ...(await importOriginal<typeof import("./DatasetHistory")>()),
  DatasetHistory: () => null,
}));
afterEach(() => vi.restoreAllMocks());
const context = () =>
  ({
    trade: vi.fn(),
    query: vi.fn(),
    snapshot: {
      market_endpoint: { host: "market", port: 7450, fingerprint: "f", instruments: [] },
    },
  }) as unknown as TerminalContext;
const source = (provider: string, tables = [Table.DAILY]) => ({
  provider,
  tables,
  exchanges: ["SHFE"],
});
const task = (id: string, provider: string) =>
  create(AcquisitionTaskSchema, {
    request: { id, provider, table: Table.DAILY, firstDay: "2026-10-30", lastDay: "2026-10-30" },
    state: TaskState.RUNNING,
    items: 1n,
  });
function serve(canManage = true) {
  request.mockImplementation(async (body: { case: string; value: { provider?: string } }) => {
    switch (body.case) {
      case "listSources":
        return { sources: [source("tushare"), source("shfe", [Table.SETTLEMENT])], canManage };
      case "listDatasets":
        return { datasets: [] };
      case "getDatasetSummary":
        return { tables: [] };
      case "listAcquisitionTasks":
        return {
          tasks: body.value.provider === "tushare" ? [task("first", "tushare")] : [],
          nextId: "",
        };
      default:
        return {};
    }
  });
}

test("switching providers submits directly to Market without configuring Lab or credentials", async () => {
  serve();
  const props = context();
  render(<Coverage {...props} />);
  await screen.findByRole("option", { name: "tushare" });
  fireEvent.change(screen.getByLabelText("数据源"), { target: { value: "tushare" } });
  await waitFor(() =>
    expect((screen.getByRole("button", { name: "开始采集" }) as HTMLButtonElement).disabled).toBe(
      true,
    ),
  );
  fireEvent.change(screen.getByLabelText("数据源"), { target: { value: "shfe" } });
  expect((screen.getByLabelText("表") as HTMLSelectElement).value).toBe(String(Table.SETTLEMENT));
  await waitFor(() =>
    expect((screen.getByRole("button", { name: "开始采集" }) as HTMLButtonElement).disabled).toBe(
      false,
    ),
  );
  fireEvent.click(screen.getByRole("button", { name: "开始采集" }));
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith(
      expect.objectContaining({
        case: "startAcquisition",
        value: expect.objectContaining({
          provider: "shfe",
          table: Table.SETTLEMENT,
          exchanges: ["SHFE"],
        }),
      }),
    ),
  );
  expect(props.trade).not.toHaveBeenCalled();
});

test("read-only clients see shared tasks but cannot start or cancel them", async () => {
  serve(false);
  render(<Coverage {...context()} />);
  await screen.findByText("当前连接只有读取权限，不能启动或取消采集。");
  fireEvent.change(screen.getByLabelText("数据源"), { target: { value: "tushare" } });
  await screen.findByRole("table", { name: "采集任务" });
  expect(screen.queryByRole("button", { name: "取消" })).toBeNull();
  fireEvent.change(screen.getByLabelText("数据源"), { target: { value: "shfe" } });
  expect((screen.getByRole("button", { name: "开始采集" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
});

test("stored disabled sources remain visible and coverage uses the summary's version", async () => {
  request.mockImplementation(async (body: { case: string; value: { afterId?: string } }) => {
    switch (body.case) {
      case "listSources":
        return { sources: [], canManage: false };
      case "listDatasets":
        return { datasets: [{ provider: "shfe" }, { provider: "tushare" }] };
      case "getDatasetSummary":
        return { dataset: { version: "fixed-version" }, tables: [] };
      case "getDatasetCoverage":
        return {
          products: [{ product: "SHFE/rb", months: [{ month: "2026-10", days: 1, openDays: 0 }] }],
        };
      case "listAcquisitionTasks":
        return body.value.afterId
          ? { tasks: [task("remote-task", "shfe")], nextId: "" }
          : { tasks: [], nextId: "first" };
      default:
        return {};
    }
  });
  render(<Coverage {...context()} />);
  await screen.findByRole("rowheader", { name: "SHFE/rb" });
  await screen.findByRole("option", { name: "tushare" });
  expect(request).toHaveBeenCalledWith({
    case: "getDatasetCoverage",
    value: { provider: "shfe", version: "fixed-version" },
  });
  expect(screen.queryByRole("button", { name: "开始采集" })).toBeNull();
  expect(screen.getByRole("table", { name: "采集任务" })).toBeTruthy();
});

test("polls shared tasks even when this client has no active acquisition", async () => {
  let poll: () => void = () => {};
  const interval = globalThis.setInterval;
  vi.spyOn(globalThis, "setInterval").mockImplementation((callback, delay, ...args) => {
    if (delay === 3000) poll = callback as () => void;
    return interval(callback, delay, ...args);
  });
  let remote = false;
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "listSources":
        return { sources: [source("shfe")], canManage: true };
      case "listDatasets":
        return { datasets: [] };
      case "getDatasetSummary":
        return { tables: [] };
      case "listAcquisitionTasks":
        return { tasks: remote ? [task("other-client", "shfe")] : [], nextId: "" };
      default:
        return {};
    }
  });
  render(<Coverage {...context()} />);
  await screen.findByRole("button", { name: "开始采集" });
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      case: "listAcquisitionTasks",
      value: { provider: "shfe", afterId: "" },
    }),
  );
  remote = true;
  await act(async () => {
    poll();
  });
  await screen.findByRole("table", { name: "采集任务" });
  fireEvent.click(screen.getByRole("button", { name: "取消" }));
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      case: "cancelAcquisition",
      value: { id: "other-client" },
    }),
  );
});

test("recorded minutes publish through Market with a contract and explicit refresh", async () => {
  serve();
  const current = request.getMockImplementation()!;
  request.mockImplementation(async body =>
    body.case === "listSources"
      ? { sources: [source("recorder", [Table.MINUTES])], canManage: true }
      : current(body),
  );
  render(<Coverage {...context()} />);
  await screen.findByRole("option", { name: "自录" });
  const publish = screen.getByRole("button", { name: "发布录制分钟" }) as HTMLButtonElement;
  expect(publish.disabled).toBe(true);
  fireEvent.change(screen.getByLabelText("其他合约"), { target: { value: "SHFE/rb/2027-01" } });
  fireEvent.click(screen.getByRole("button", { name: "添加" }));
  fireEvent.click(screen.getByLabelText("重新发布已有分区，保留旧数据集版本"));
  fireEvent.click(publish);
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith(
      expect.objectContaining({
        case: "startAcquisition",
        value: expect.objectContaining({
          provider: "recorder",
          table: Table.MINUTES,
          contracts: ["SHFE/rb/2027-01"],
          refetch: true,
        }),
      }),
    ),
  );
});

// A tushare source with daily and minute bars, and what it already holds.
function serveCoverage() {
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "listSources":
        return { sources: [source("tushare", [Table.DAILY, Table.MINUTES])], canManage: true };
      case "listDatasets":
        return { datasets: [] };
      case "getDatasetSummary":
        return { dataset: { version: "v" }, tables: [] };
      case "getDatasetCoverage":
        return {
          products: [
            { product: "SHFE/rb", months: [{ month: "2026-09", days: 10, openDays: 21 }] },
          ],
          contracts: [
            { contract: "SHFE/cu/2026-12", months: [{ month: "2026-10", days: 3 }] },
            { contract: "SHFE/rb/2027-01", months: [{ month: "2026-10", days: 3 }] },
            { contract: "SHFE/rb/2027-02", months: [{ month: "2026-10", days: 3 }] },
          ],
        };
      case "listAcquisitionTasks":
        return { tasks: [], nextId: "" };
      default:
        return {};
    }
  });
}
const submitted = () =>
  request.mock.calls.map(([body]) => body).find(body => body.case === "startAcquisition")?.value;

test("a minute acquisition covers every contract chosen for a product", async () => {
  serveCoverage();
  render(<Coverage {...context()} />);
  await screen.findByRole("table", { name: "分钟线覆盖" });
  fireEvent.change(screen.getByLabelText("表"), { target: { value: String(Table.MINUTES) } });
  const picker = screen.getByRole("group", { name: "合约（分钟线必填）" });
  fireEvent.change(within(picker).getByLabelText("品种"), { target: { value: "SHFE/rb" } });
  fireEvent.click(within(picker).getByRole("button", { name: "选中本品种全部合约" }));
  fireEvent.click(screen.getByRole("button", { name: "开始采集" }));
  await waitFor(() =>
    expect(submitted()).toMatchObject({
      table: Table.MINUTES,
      exchanges: [],
      contracts: ["SHFE/rb/2027-01", "SHFE/rb/2027-02"],
    }),
  );
});

test("a coverage gap fills the acquisition with its month", async () => {
  serveCoverage();
  render(<Coverage {...context()} />);
  fireEvent.click(await screen.findByRole("button", { name: "补采 SHFE/rb 2026-09" }));
  fireEvent.click(screen.getByRole("button", { name: "开始采集" }));
  await waitFor(() =>
    expect(submitted()).toMatchObject({
      table: Table.DAILY,
      exchanges: ["SHFE"],
      contracts: [],
      firstDay: "2026-09-01",
      lastDay: "2026-09-30",
    }),
  );
  request.mockClear();
  const minutes = screen.getByRole("region", { name: "分钟线覆盖" });
  fireEvent.change(within(minutes).getByLabelText("品种"), { target: { value: "SHFE/rb" } });
  fireEvent.click(within(minutes).getByRole("button", { name: "补采 SHFE/rb/2027-01 2026-10" }));
  fireEvent.click(screen.getByRole("button", { name: "开始采集" }));
  await waitFor(() =>
    expect(submitted()).toMatchObject({
      table: Table.MINUTES,
      contracts: ["SHFE/rb/2027-01"],
      firstDay: "2026-10-01",
      lastDay: "2026-10-31",
    }),
  );
});

test("each refresh reads one page of the source's tasks; earlier ones are read on request", async () => {
  const page = Array.from({ length: 20 }, (_, i) => task(`task-${i}`, "tushare"));
  request.mockImplementation(async (body: { case: string; value: { afterId?: string } }) => {
    switch (body.case) {
      case "listSources":
        return { sources: [source("tushare")], canManage: false };
      case "listDatasets":
        return { datasets: [] };
      case "getDatasetSummary":
        return { tables: [] };
      case "listAcquisitionTasks":
        return body.value.afterId
          ? { tasks: [task("oldest", "tushare")], nextId: "" }
          : { tasks: page, nextId: "task-19" };
      default:
        return {};
    }
  });
  render(<Coverage {...context()} />);
  fireEvent.click(await screen.findByRole("button", { name: "更早的任务" }));
  const reads = () =>
    request.mock.calls.filter(([body]) => body.case === "listAcquisitionTasks").map(([b]) => b);
  expect(reads()).toEqual([
    { case: "listAcquisitionTasks", value: { provider: "tushare", afterId: "" } },
  ]);
  await screen.findByText("oldest");
  expect(reads().at(-1)).toEqual({
    case: "listAcquisitionTasks",
    value: { provider: "tushare", afterId: "task-19" },
  });
  expect(screen.queryByRole("button", { name: "更早的任务" })).toBeNull();
});
