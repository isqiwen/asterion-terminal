import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { afterEach, expect, test, vi } from "vitest";
import { AcquisitionTaskSchema, Table, TaskState } from "../../src/generated/asterion/history_pb";
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
const task = (id: string) =>
  create(AcquisitionTaskSchema, {
    request: { id, table: Table.DAILY, firstDay: "2026-10-30", lastDay: "2026-10-30" },
    state: TaskState.RUNNING,
    items: 1n,
  });
function serve(canManage = true, tables = [Table.DAILY, Table.PRICE_TICKS]) {
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables, exchanges: ["SHFE"], canManage };
      case "getDatasetSummary":
        return { tables: [] };
      case "listAcquisitionTasks":
        return { tasks: [task("first")], nextId: "" };
      default:
        return {};
    }
  });
}
const started = () =>
  request.mock.calls
    .map(([body]) => body)
    .filter(body => body.case === "startAcquisition")
    .map(body => [body.value.table, body.value.exchanges]);

test("clients choose no source, and market data also asks for its price ticks", async () => {
  serve();
  const props = context();
  render(<Coverage {...props} />);
  const start = (await screen.findByRole("button", { name: "开始采集" })) as HTMLButtonElement;
  expect(screen.queryByLabelText("数据源")).toBeNull();
  await waitFor(() => expect(start.disabled).toBe(false));
  fireEvent.click(start);
  await waitFor(() =>
    expect(started()).toEqual([
      [Table.DAILY, ["SHFE"]],
      [Table.PRICE_TICKS, ["SHFE"]],
    ]),
  );
  expect(request.mock.calls.flat().some(body => "provider" in (body.value ?? {}))).toBe(false);
  expect(props.trade).not.toHaveBeenCalled();
});

test("read-only clients see shared tasks but cannot start or cancel them", async () => {
  serve(false);
  render(<Coverage {...context()} />);
  await screen.findByText("当前连接只有读取权限，不能启动或取消采集。");
  await screen.findByRole("table", { name: "采集任务" });
  expect(screen.queryByRole("button", { name: "取消" })).toBeNull();
  expect((screen.getByRole("button", { name: "开始采集" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
});

test("coverage uses the summary's version and no form appears without a source", async () => {
  request.mockImplementation(async (body: { case: string; value: { afterId?: string } }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables: [], exchanges: [], canManage: false };
      case "getDatasetSummary":
        return { dataset: { version: "fixed-version" }, tables: [] };
      case "getDatasetCoverage":
        return {
          products: [{ product: "SHFE/rb", months: [{ month: "2026-10", days: 1, openDays: 0 }] }],
        };
      case "listAcquisitionTasks":
        return body.value.afterId
          ? { tasks: [task("remote-task")], nextId: "" }
          : { tasks: [], nextId: "first" };
      default:
        return {};
    }
  });
  render(<Coverage {...context()} />);
  await screen.findByRole("rowheader", { name: "SHFE/rb" });
  expect(request).toHaveBeenCalledWith({
    case: "getDatasetCoverage",
    value: { version: "fixed-version" },
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
      case "getAcquisitionOptions":
        return { tables: [Table.DAILY], exchanges: ["SHFE"], canManage: true };
      case "getDatasetSummary":
        return { tables: [] };
      case "listAcquisitionTasks":
        return { tasks: remote ? [task("other-client")] : [], nextId: "" };
      default:
        return {};
    }
  });
  render(<Coverage {...context()} />);
  await screen.findByRole("button", { name: "开始采集" });
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith({
      case: "listAcquisitionTasks",
      value: { afterId: "" },
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

// Daily and minute bars on offer, and what the dataset already holds.
function serveCoverage() {
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables: [Table.DAILY, Table.MINUTES], exchanges: ["SHFE"], canManage: true };
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

test("each refresh reads one page of tasks; earlier ones are read on request", async () => {
  const page = Array.from({ length: 20 }, (_, i) => task(`task-${i}`));
  request.mockImplementation(async (body: { case: string; value: { afterId?: string } }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables: [Table.DAILY], exchanges: ["SHFE"], canManage: false };
      case "getDatasetSummary":
        return { tables: [] };
      case "listAcquisitionTasks":
        return body.value.afterId
          ? { tasks: [task("oldest")], nextId: "" }
          : { tasks: page, nextId: "task-19" };
      default:
        return {};
    }
  });
  render(<Coverage {...context()} />);
  fireEvent.click(await screen.findByRole("button", { name: "更早的任务" }));
  const reads = () =>
    request.mock.calls.filter(([body]) => body.case === "listAcquisitionTasks").map(([b]) => b);
  expect(reads()).toEqual([{ case: "listAcquisitionTasks", value: { afterId: "" } }]);
  await screen.findByText("oldest");
  expect(reads().at(-1)).toEqual({
    case: "listAcquisitionTasks",
    value: { afterId: "task-19" },
  });
  expect(screen.queryByRole("button", { name: "更早的任务" })).toBeNull();
});
