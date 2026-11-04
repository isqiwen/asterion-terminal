import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { afterEach, expect, test, vi } from "vitest";
import { AcquisitionTaskSchema, Table, TaskState } from "../../src/generated/asterion/market_pb";
import type { TerminalContext } from "../contract";
import { Coverage } from "./Coverage";

const request = vi.hoisted(() => vi.fn());
vi.mock("../contract", async importOriginal => ({
  ...(await importOriginal<typeof import("../contract")>()),
  marketData: request,
  translate: (_namespace: string, key: string) => key,
}));
// The comparison has its own test.
vi.mock("./MinuteComparison", () => ({ MinuteComparison: () => null }));
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
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "listSources":
        return { sources: [source("tushare"), source("shfe", [Table.SETTLEMENT])], canManage };
      case "listDatasets":
        return { datasets: [] };
      case "getDatasetSummary":
        return { tables: [] };
      case "listAcquisitionTasks":
        return { tasks: [task("first", "tushare")], nextId: "" };
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
    expect(request).toHaveBeenCalledWith({ case: "listAcquisitionTasks", value: { afterId: "" } }),
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
  fireEvent.change(screen.getByLabelText("合约（分钟线必填）"), {
    target: { value: "SHFE/rb/2027-01" },
  });
  fireEvent.click(screen.getByLabelText("重新发布已有分区，保留旧数据集版本"));
  fireEvent.click(publish);
  await waitFor(() =>
    expect(request).toHaveBeenCalledWith(
      expect.objectContaining({
        case: "startAcquisition",
        value: expect.objectContaining({
          provider: "recorder",
          table: Table.MINUTES,
          contract: "SHFE/rb/2027-01",
          refetch: true,
        }),
      }),
    ),
  );
});
