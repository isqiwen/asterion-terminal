import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { afterEach, expect, test, vi } from "vitest";
import {
  AcquisitionTaskSchema,
  DatasetCoverageSchema,
  Table,
  TaskState,
} from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { openPopover } from "../../src/testing/popover";
import { tradingRequest } from "../../src/bridge/trading";
import { ResponseSchema } from "../../src/generated/asterion/trading_pb";
import { TradingAccountsProvider } from "../../src/host/trading-accounts";
import { Dataset } from "./DatasetView";

vi.mock("../../src/bridge/trading", () => ({ tradingRequest: vi.fn() }));

const request = vi.hoisted(() => vi.fn());
vi.mock("../../src/bridge/warehouse-data", () => ({
  warehouseData: request,
}));
vi.mock("../../src/i18n", async importOriginal => ({
  ...(await importOriginal<typeof import("../../src/i18n")>()),
  translate: (_namespace: string, key: string, values?: Record<string, unknown>) =>
    key.replace(/\{(\w+)\}/g, (_, name: string) => String(values?.[name])),
}));
afterEach(() => vi.restoreAllMocks());

const context = (params?: Record<string, string>) =>
  ({
    trade: vi.fn(),
    navigate: vi.fn(),
    snapshot: { warehouse_endpoint: { host: "market", port: 7450, fingerprint: "f" } },
    workspaceParams: params,
  }) as unknown as TerminalContext;

const view = (props: TerminalContext) =>
  render(
    <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.data-dataset">
      <Dataset {...props} />
    </WorkspaceDraftScope>,
  );

const older = "a".repeat(64);
const current = "b".repeat(64);
const task = (id: string, table = Table.DAILY, startedMs = 1n) =>
  create(AcquisitionTaskSchema, {
    request: { id, table, firstDay: "2026-10-30", lastDay: "2026-10-30" },
    state: TaskState.RUNNING,
    items: 2n,
    itemsDone: 1n,
    startedMs,
  });

function serve({
  tables = [Table.DAILY, Table.MINUTES, Table.SETTLEMENT, Table.PRICE_TICKS],
  tasks = () => ({ tasks: [] as unknown[], nextId: "" }),
}: {
  tables?: Table[];
  tasks?: (afterId: string) => { tasks: unknown[]; nextId: string };
} = {}) {
  request.mockImplementation(
    async (body: { case: string; value: { version?: string; afterId?: string } }) => {
      switch (body.case) {
        case "getAcquisitionOptions":
          return { tables: tables.map(table => ({ table, exchanges: ["CFFEX", "SHFE"] })) };
        case "getDatasetSummary":
          return {
            dataset: { version: body.value.version || current, createdMs: 3n },
            tables: [
              {
                table: Table.DAILY,
                rows: 5n,
                firstDay: "2026-07-01",
                lastDay: "2026-11-10",
                expectedLastDay: "2026-11-12",
                missing: 2n,
                sourceEmpty: 3n,
              },
              {
                table: Table.SETTLEMENT,
                rows: 3n,
                firstDay: "2026-11-10",
                lastDay: "2026-11-12",
                expectedLastDay: "2026-11-12",
              },
              {
                table: Table.PRICE_TICKS,
                rows: 4n,
                firstDay: "2026-07-01",
                lastDay: "2026-11-12",
                expectedLastDay: "2026-11-12",
              },
            ],
          };
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
                      contracts: ["SHFE/rb/2026-10", "SHFE/rb/2027-01"],
                      months: [
                        { month: "2026-07", days: 23, openDays: 23 },
                        { month: "2026-09", days: 10, openDays: 21 },
                        { month: "2026-10", days: 16, openDays: 16 },
                      ],
                    },
                    {
                      product: "SHFE/au",
                      months: [
                        { month: "2026-09", days: 18, openDays: 21, pendingDays: 2 },
                        { month: "2026-10", days: 10, openDays: 16, emptyDays: 6 },
                      ],
                    },
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
        case "listAcquisitionTasks":
          return tasks(body.value.afterId ?? "");
        case "listDatasetVersions":
          return {
            versions: [
              { dataset: { version: current, createdMs: 3n }, added: 1 },
              { dataset: { version: older, createdMs: 2n }, added: 2 },
            ],
            nextVersion: "",
          };
        case "compareDatasetVersions":
          return { added: 1, replaced: 0, removed: 0 };
        case "startUpdate":
          return { tasks: [task("update-1-daily"), task("update-1-minutes", Table.MINUTES)] };
        default:
          return {};
      }
    },
  );
}

const submitted = () =>
  request.mock.calls
    .map(([body]) => body)
    .filter(body => body.case === "startAcquisition")
    .map(body => body.value);
const startInDialog = () =>
  fireEvent.click(within(screen.getByRole("dialog")).getByRole("button", { name: "开始采集" }));
const show = (prefix: string) =>
  fireEvent.click(
    within(screen.getByRole("group", { name: "数据" })).getByRole("button", {
      name: new RegExp(`^${prefix}`),
    }),
  );

test("market series cards show freshness, acquisition defaults to canonical exchange order, and update brings all current", async () => {
  serve();
  view(context());
  const series = await screen.findByRole("group", { name: "数据" });
  const daily = within(series).getByRole("button", { name: /^日线/ });
  const minutes = within(series).getByRole("button", { name: /^分钟线/ });
  expect(daily.textContent).toContain("3 个品种 · 5 行");
  expect(daily.textContent).not.toContain("个合约");
  expect(daily.textContent).toContain("缺 2 个交易日");
  expect(daily.textContent).toContain("应到 2026-11-12");
  expect(daily.textContent).toContain("另有 3 个交易日来源无数据");
  expect(minutes.textContent).toContain("未采集");
  expect(screen.queryByText(/回测参数/)).toBeNull();

  fireEvent.click(screen.getByRole("button", { name: "采集" }));
  const dialog = screen.getByRole("dialog", { name: "采集 日线" });
  expect(within(dialog).queryByRole("group", { name: "品种或合约" })).toBeNull();
  expect((within(dialog).getByRole("checkbox", { name: "SHFE" }) as HTMLInputElement).checked).toBe(
    true,
  );
  expect(
    (within(dialog).getByRole("checkbox", { name: "CFFEX" }) as HTMLInputElement).checked,
  ).toBe(false);
  fireEvent.click(within(dialog).getByRole("button", { name: "取消" }));

  fireEvent.click(screen.getByRole("button", { name: "更新到最新" }));
  await screen.findByText("已提交 2 个更新任务");
  expect(request).toHaveBeenCalledWith({ case: "startUpdate", value: {} });
});

test("hovering shows detail only for months their number and colour cannot explain", async () => {
  serve();
  view(context());
  const matrix = await screen.findByRole("table", { name: "日线覆盖" });
  const cellsOf = (name: RegExp) =>
    within(within(matrix).getByRole("row", { name })).getAllByRole("cell");
  const gold = cellsOf(/SHFE\/au/);
  const rebar = cellsOf(/SHFE\/rb/);
  const tip = screen.getByRole("tooltip");
  fireEvent.pointerOver(gold[8]!);
  expect(tip.textContent).toBe("18 / 21 · 缺 1 天 · 2 天待更新");
  fireEvent.pointerOver(gold[9]!);
  expect(tip.textContent).toBe("10 / 16 · 6 天来源无数据");
  // Complete, gap-only and blank months say nothing more on hover.
  for (const index of [9, 8, 11]) {
    fireEvent.pointerOver(rebar[index]!);
    expect(tip.textContent).toBe("");
  }
  fireEvent.pointerOver(gold[9]!);
  fireEvent.pointerLeave(screen.getByRole("region", { name: "日线覆盖" }));
  expect(tip.textContent).toBe("");
});

test("monthly coverage shows localized product names and gaps submit a single bar acquisition", async () => {
  serve();
  view(context());
  const matrix = await screen.findByRole("table", { name: "日线覆盖" });
  const row = within(matrix).getByRole("row", { name: /SHFE\/rb/ });
  expect(within(row).getByRole("rowheader").textContent).toContain("螺纹钢");
  expect(within(row).getByRole("rowheader").textContent).not.toContain("个合约");
  const cells = within(row).getAllByRole("cell");
  expect(cells.slice(6, 10).map(cell => cell.getAttribute("data-cell"))).toEqual([
    "full",
    "missing",
    "partial",
    "full",
  ]);
  expect(cells[8]!.textContent).toBe("10/21");
  expect(cells[5]!.getAttribute("data-cell")).toBe("unacquired");
  expect(cells[10]!.getAttribute("data-cell")).toBe("unacquired");
  expect(cells[11]!.getAttribute("data-cell")).toBe("future");
  const key = document.querySelector(".data-legend")!.textContent;
  expect(key).toContain("有缺口");
  expect(key).not.toMatch(/未采集|未到来/);
  expect(screen.queryByRole("button", { name: /^补采 SHFE\/rb 2026-12/ })).toBeNull();
  expect(screen.queryByRole("button", { name: /^补采 SHFE\/au 2026-10/ })).toBeNull();
  const silver = within(within(matrix).getByRole("row", { name: /SHFE\/ag/ })).getAllByRole("cell");
  expect(silver[9]!.getAttribute("data-cell")).toBe("pending");
  expect(silver[9]!.textContent).toBe("12/16");

  // Past unacquired months (before the earliest stored month) are also clickable.
  fireEvent.click(screen.getByRole("button", { name: "补采 SHFE/rb 2026-06" }));
  expect(
    (within(screen.getByRole("dialog")).getByLabelText("开始日期") as HTMLInputElement).value,
  ).toBe("2026-06-01");
  fireEvent.click(within(screen.getByRole("dialog")).getByRole("button", { name: "取消" }));

  fireEvent.click(screen.getByRole("button", { name: "补采 SHFE/rb 2026-08" }));
  startInDialog();
  await waitFor(() => expect(submitted().map(item => item.table)).toEqual([Table.DAILY]));
  expect(submitted()[0]).toMatchObject({
    exchanges: ["SHFE"],
    instruments: [],
    firstDay: "2026-08-01",
    lastDay: "2026-08-31",
  });
});

test("the key lists every state whatever is on screen, and delisted only for contracts", async () => {
  serve();
  view(context());
  await screen.findByRole("table", { name: "日线覆盖" });
  fireEvent.change(screen.getByRole("searchbox"), { target: { value: "SHFE/rb" } });
  const key = () =>
    [...document.querySelectorAll(".data-legend > span")].map(item => item.textContent!.trim());
  const daily = ["齐全", "待更新", "有缺口", "整月缺失", "来源无数据"];
  expect(key()).toEqual(daily);
  show("分钟线");
  await screen.findByRole("table", { name: "分钟线覆盖" });
  expect(key()).toEqual([...daily, "已摘牌"]);
});

test("a coverage cell decides the refetch and the acquire button leaves it to the user", async () => {
  serve();
  view(context());
  await screen.findByRole("table", { name: "日线覆盖" });
  const option = () =>
    within(screen.getByRole("dialog")).queryByRole<HTMLInputElement>("checkbox", {
      name: "重新采集已有分区",
    });
  const cases = [
    [/^补采 SHFE\/rb 2026-06/, false], // not acquired
    [/^补采 SHFE\/ag 2026-10/, false], // awaiting update
    [/^补采 SHFE\/rb 2026-08/, true], // whole month missing
    [/^补采 SHFE\/rb 2026-09/, true], // gaps
  ] as const;
  for (const [index, [name, refetch]] of cases.entries()) {
    fireEvent.click(screen.getByRole("button", { name }));
    expect(option()).toBeNull();
    startInDialog();
    await waitFor(() => expect(submitted()).toHaveLength(index + 1));
    expect(submitted()[index]).toMatchObject({ exchanges: ["SHFE"], refetch });
    await waitFor(() => expect(screen.queryByRole("dialog")).toBeNull());
  }

  fireEvent.click(screen.getByRole("button", { name: "采集" }));
  expect(option()!.checked).toBe(false);
});

test("the matrix lists the products held or watched until all products are asked for", async () => {
  serve();
  const props = context();
  props.snapshot!.market_endpoint = {
    host: "market",
    port: 7440,
    fingerprint: "f",
    instruments: [{ exchange: "SHFE", symbol: "zn2612" }],
  };
  view(props);
  const matrix = await screen.findByRole("table", { name: "日线覆盖" });
  expect(within(matrix).getByRole("row", { name: /SHFE\/zn/ })).toBeTruthy();
  expect(within(matrix).queryByRole("row", { name: /SHFE\/pb/ })).toBeNull();
  fireEvent.click(screen.getByRole("checkbox", { name: "全部品种" }));
  expect(within(matrix).getByRole("row", { name: /SHFE\/pb/ })).toBeTruthy();
});

test("minute acquisition submits one task, prerequisite requests show live progress, and update batches cancel together", async () => {
  let poll: () => void = () => {};
  const interval = globalThis.setInterval;
  vi.spyOn(globalThis, "setInterval").mockImplementation((callback, delay, ...args) => {
    if (delay === 3000) poll = callback as () => void;
    return interval(callback, delay, ...args);
  });
  let runningPhase: "idle" | "prerequisites" | "update" = "idle";
  serve({
    tasks: () => ({
      tasks:
        runningPhase === "prerequisites"
          ? [
              create(AcquisitionTaskSchema, {
                request: {
                  id: "acq-123456789abc",
                  table: Table.MINUTES,
                  instruments: ["SHFE/rb"],
                },
                state: TaskState.RUNNING,
                requestsSent: 3n,
                startedMs: 4n,
              }),
            ]
          : runningPhase === "update"
            ? [task("update-1-daily", Table.DAILY, 5n), task("update-1-minutes", Table.MINUTES, 5n)]
            : [],
      nextId: "",
    }),
  });
  const props = context();
  view(props);
  await screen.findByRole("table", { name: "日线覆盖" });
  show("分钟线");
  await screen.findByRole("button", { name: /^补采 SHFE\/cu\/2026-12 2026-10/ });
  const minuteMatrix = screen.getByRole("table", { name: "分钟线覆盖" });
  // Once data is held, contracts without any wait behind the toggle.
  expect(within(minuteMatrix).queryByRole("row", { name: /SHFE\/rb\/2026-10/ })).toBeNull();
  fireEvent.click(screen.getByRole("checkbox", { name: "全部品种" }));
  const expiredCells = within(
    within(minuteMatrix).getByRole("row", { name: /SHFE\/rb\/2026-10/ }),
  ).getAllByRole("cell");
  expect(expiredCells[10]!.getAttribute("data-cell")).toBe("delisted");
  expect(expiredCells[10]!.textContent).toBe("—");
  const activeCells = within(
    within(minuteMatrix).getByRole("row", { name: /SHFE\/rb\/2027-01/ }),
  ).getAllByRole("cell");
  expect(activeCells[11]!.getAttribute("data-cell")).toBe("future");
  expect(activeCells[11]!.textContent).toBe("");

  fireEvent.click(screen.getByRole("button", { name: /^补采 SHFE\/cu\/2026-12 2026-10/ }));
  let dialog = screen.getByRole("dialog", { name: "采集 分钟线" });
  expect(within(dialog).getByRole("button", { name: "移除 SHFE/cu/2026-12" })).toBeTruthy();
  expect(
    (within(dialog).getByRole("checkbox", { name: /^整个品种 · SHFE\/cu/ }) as HTMLInputElement)
      .indeterminate,
  ).toBe(true);
  fireEvent.click(within(dialog).getByRole("button", { name: "取消" }));

  fireEvent.click(screen.getByRole("button", { name: "采集" }));
  dialog = screen.getByRole("dialog", { name: "采集 分钟线" });
  expect(within(dialog).queryByLabelText("数据源")).toBeNull();
  const picker = within(dialog).getByRole("group", { name: "品种或合约" });
  fireEvent.click(within(picker).getByRole("button", { name: /^SHFE\/rb/ }));
  expect(within(picker).getByRole("checkbox", { name: /SHFE\/rb\/2026-10/ })).toBeTruthy();
  fireEvent.click(within(picker).getByRole("checkbox", { name: /^整个品种 · SHFE\/rb/ }));
  startInDialog();
  await waitFor(() => expect(submitted().map(item => item.table)).toEqual([Table.MINUTES]));
  expect(submitted()[0]).toMatchObject({ instruments: ["SHFE/rb"] });
  expect(props.trade).not.toHaveBeenCalled();

  runningPhase = "prerequisites";
  await act(async () => {
    poll();
  });
  const trigger = screen.getByRole("button", { name: /^最近活动/ });
  expect(trigger.textContent).toContain("1 个任务进行中");
  openPopover(trigger);
  let activity = screen.getByRole("table", { name: "最近活动" });
  let taskRows = within(activity)
    .getAllByRole("row")
    .filter(row => row.getAttribute("data-kind") === "task");
  expect(taskRows).toHaveLength(1);
  expect(taskRows[0]!.textContent).toContain("采集 分钟线");
  expect(taskRows[0]!.textContent).toContain("已请求 3 次");

  runningPhase = "update";
  await act(async () => {
    poll();
  });
  activity = screen.getByRole("table", { name: "最近活动" });
  taskRows = within(activity)
    .getAllByRole("row")
    .filter(row => row.getAttribute("data-kind") === "task");
  expect(taskRows).toHaveLength(1);
  expect(taskRows[0]!.textContent).toContain("更新到最新");
  expect(taskRows[0]!.textContent).toContain("2 / 4 个分区");

  fireEvent.click(within(taskRows[0]!).getByRole("button", { name: "取消" }));
  await waitFor(() =>
    expect(
      request.mock.calls
        .filter(([body]) => body.case === "cancelAcquisition")
        .map(([body]) => body.value.id),
    ).toEqual(["update-1-daily", "update-1-minutes"]),
  );
});

test("versions inspect historical coverage in place and pagination loads earlier tasks on demand", async () => {
  const page = Array.from({ length: 20 }, (_, i) =>
    task(`task-${i}`, Table.DAILY, BigInt(100 - i)),
  );
  serve({
    tasks: afterId =>
      afterId
        ? { tasks: [task("oldest", Table.DAILY, 0n)], nextId: "" }
        : { tasks: page, nextId: "task-19" },
  });
  const props = context();
  view(props);
  openPopover(await screen.findByRole("button", { name: /^最近活动/ }));
  const activity = await screen.findByRole("table", { name: "最近活动" });
  await waitFor(() => expect(within(activity).getAllByRole("row")).toHaveLength(20));
  fireEvent.click(screen.getByRole("button", { name: "更早的活动" }));
  await waitFor(() =>
    expect(
      within(screen.getByRole("table", { name: "最近活动" })).getAllByRole("row"),
    ).toHaveLength(23),
  );
  const olderRow = within(screen.getByRole("table", { name: "最近活动" })).getByRole("row", {
    name: new RegExp(older.slice(0, 8)),
  });
  fireEvent.click(within(olderRow).getByRole("button", { name: "查看" }));
  expect(props.navigate).toHaveBeenCalledWith("workspace.data-dataset", {
    params: { version: older },
  });
  await screen.findByRole("rowheader", { name: /^SHFE\/cu\b/ });
  expect(screen.queryByRole("button", { name: "采集" })).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "回到最新数据" }));
  await screen.findByRole("rowheader", { name: /^SHFE\/rb\b/ });
});

test("without an acquisition source nothing offers acquisition", async () => {
  serve({ tables: [] });
  view(context());
  await screen.findByRole("rowheader", { name: /^SHFE\/rb\b/ });
  expect(screen.queryByRole("button", { name: "采集" })).toBeNull();
  expect(screen.queryByRole("button", { name: /^补采/ })).toBeNull();
  expect(screen.queryByRole("button", { name: "更新到最新" })).toBeNull();
});

test("an empty warehouse offers exchanges, continuous years, and clickable rows for both daily and minutes", async () => {
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return {
          tables: [Table.DAILY, Table.MINUTES].map(table => ({
            table,
            exchanges: ["CFFEX", "SHFE"],
          })),
        };
      case "getDatasetSummary":
        return { tables: [] };
      case "listAcquisitionTasks":
        return { tasks: [], nextId: "" };
      case "listDatasetVersions":
        return { versions: [], nextVersion: "" };
      default:
        return {};
    }
  });
  view(context());
  await screen.findByRole("table", { name: "日线覆盖" });
  const exchangeSelect = screen.getByLabelText("交易所") as HTMLSelectElement;
  expect([...exchangeSelect.options].map(item => item.value)).toEqual(["", "SHFE", "CFFEX"]);
  const yearSelect = screen.getByLabelText("年份") as HTMLSelectElement;
  expect(yearSelect.options.length).toBeGreaterThanOrEqual(15);
  expect([...yearSelect.options].map(item => item.value)).toContain("2025");

  fireEvent.change(exchangeSelect, { target: { value: "CFFEX" } });
  fireEvent.change(yearSelect, { target: { value: "2025" } });
  expect(screen.getByRole("rowheader", { name: /CFFEX\/ic/ }).textContent).toContain("中证500");
  expect(screen.getByRole("button", { name: "补采 CFFEX/ic 2025-08" })).toBeTruthy();

  show("分钟线");
  await screen.findByRole("table", { name: "分钟线覆盖" });
  fireEvent.click(screen.getByRole("button", { name: "补采 CFFEX/ic 2025-08" }));
  const dialog = screen.getByRole("dialog", { name: "采集 分钟线" });
  expect(within(dialog).getByRole("button", { name: "移除 CFFEX/ic" })).toBeTruthy();
  startInDialog();
  await waitFor(() => expect(submitted().map(item => item.table)).toEqual([Table.MINUTES]));
  expect(submitted()[0]).toMatchObject({
    instruments: ["CFFEX/ic"],
    firstDay: "2025-08-01",
    lastDay: "2025-08-31",
  });
});

test("an unreachable warehouse shows an inline error with retry and settings actions", async () => {
  request.mockRejectedValue(new Error("warehouse offline"));
  const openSettings = vi.fn();
  view({ ...context(), openSettings });
  const alert = await screen.findByRole("alert");
  expect(alert.textContent).toContain("warehouse offline");
  fireEvent.click(screen.getByRole("button", { name: "数据服务设置" }));
  expect(openSettings).toHaveBeenCalledWith("warehouse");
  fireEvent.click(screen.getByRole("button", { name: "刷新" }));
  await waitFor(() => expect(request.mock.calls.length).toBeGreaterThan(2));
});

test("my products include running dominant series and kind params clear after consumption", async () => {
  serve();
  vi.mocked(tradingRequest).mockImplementation(async (_id, operation) =>
    operation.case === "getStrategies"
      ? create(ResponseSchema, {
          result: {
            case: "getStrategies",
            value: {
              instance: {
                instance: {
                  id: "dom",
                  series: [{ exchange: "SHFE", product: "zn" }],
                },
              },
            },
          },
        })
      : create(ResponseSchema, { result: { case: "getAccount", value: {} } }),
  );
  const props = context({ kind: "minutes" });
  render(
    <TradingAccountsProvider connections={[{ id: "sim", revision: "1" }]}>
      <WorkspaceDraftScope store={createWorkspaceDrafts()} namespace="workspace.data-dataset">
        <Dataset {...props} />
      </WorkspaceDraftScope>
    </TradingAccountsProvider>,
  );
  const matrix = await screen.findByRole("table", { name: "分钟线覆盖" });
  await waitFor(() => expect(within(matrix).getByRole("row", { name: /SHFE\/zn/ })).toBeTruthy());
  expect(props.navigate).toHaveBeenCalledWith("workspace.data-dataset", {});
});
