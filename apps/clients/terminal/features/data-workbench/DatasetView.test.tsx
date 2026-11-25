import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { create } from "@bufbuild/protobuf";
import { afterEach, expect, test, vi } from "vitest";
import {
  AcquisitionTaskSchema,
  DatasetCoverageSchema,
  ProductContractsSchema,
  Table,
  TaskState,
} from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { openPopover } from "../../src/testing/popover";
import { Dataset } from "./DatasetView";

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

const series = (days: number, extra: Record<string, unknown> = {}) => ({
  days,
  openDays: days,
  firstDay: "2026-07-01",
  lastDay: "2026-10-30",
  ...extra,
});

const rebar = [
  {
    contract: "SHFE/rb/2024-10",
    listDay: "2023-10-16",
    delistDay: "2024-10-15",
  },
  {
    contract: "SHFE/rb/2025-10",
    listDay: "2024-10-16",
    delistDay: "2025-10-15",
    daily: series(5, { firstDay: "2025-07-01", lastDay: "2025-10-15", openDays: 9 }),
  },
  {
    contract: "SHFE/rb/2026-10",
    listDay: "2025-10-16",
    delistDay: "2026-10-15",
    daily: series(5, { firstDay: "2025-10-16", lastDay: "2026-10-15" }),
  },
  {
    contract: "SHFE/rb/2027-01",
    listDay: "2026-01-16",
    delistDay: "2099-01-15",
    listed: true,
    daily: series(10),
    minutes: series(3),
  },
  {
    contract: "SHFE/rb/2027-02",
    listDay: "2026-02-25",
    delistDay: "2099-02-15",
    listed: true,
    daily: series(8, {
      openDays: 10,
      gapDays: 2,
      gaps: [{ first: "2026-09-10", last: "2026-09-11" }],
    }),
  },
];

const lead = [
  {
    contract: "SHFE/pb/2023-10",
    listDay: "2022-10-16",
    delistDay: "2023-10-15",
  },
  {
    contract: "SHFE/pb/2024-10",
    listDay: "2023-10-16",
    delistDay: "2024-10-15",
  },
];

function serve({
  tables = [Table.DAILY, Table.MINUTES, Table.SETTLEMENT, Table.PRICE_TICKS],
  tasks = () => ({ tasks: [] as unknown[], nextId: "" }),
}: {
  tables?: Table[];
  tasks?: (afterId: string) => { tasks: unknown[]; nextId: string };
} = {}) {
  request.mockImplementation(
    async (body: {
      case: string;
      value: { version?: string; afterId?: string; product?: string };
    }) => {
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
            ],
          };
        case "getDatasetCoverage":
          return create(
            DatasetCoverageSchema,
            body.value.version === older
              ? {
                  products: [
                    {
                      product: "SHFE/cu",
                      contracts: 1,
                      listed: 1,
                      daily: { held: 1, firstDay: "2026-10-01", lastDay: "2026-10-30" },
                    },
                  ],
                }
              : {
                  products: [
                    {
                      product: "SHFE/ag",
                      contracts: 2,
                      listed: 2,
                      daily: {
                        held: 2,
                        complete: 1,
                        listedComplete: 1,
                        firstDay: "2026-07-01",
                        lastDay: "2026-10-30",
                      },
                    },
                    {
                      product: "SHFE/au",
                      contracts: 2,
                      listed: 2,
                      daily: { held: 2, pending: 1, firstDay: "2026-07-01", lastDay: "2026-10-30" },
                    },
                    { product: "SHFE/pb", contracts: lead.length },
                    { product: "SHFE/zn", contracts: 1, listed: 1 },
                    {
                      product: "SHFE/rb",
                      contracts: rebar.length,
                      listed: 2,
                      daily: {
                        held: 4,
                        complete: 2,
                        listedComplete: 1,
                        withGaps: 1,
                        firstDay: "2025-07-01",
                        lastDay: "2026-10-30",
                        gaps: [{ first: "2026-09-10", last: "2026-09-11" }],
                      },
                      minutes: { held: 1, firstDay: "2026-07-01", lastDay: "2026-10-30" },
                    },
                  ],
                },
          );
        case "getProductCoverage":
          return create(ProductContractsSchema, {
            contracts:
              body.value.product === "SHFE/rb"
                ? rebar
                : body.value.product === "SHFE/pb"
                  ? lead
                  : [],
          });
        case "listAcquisitionTasks":
          return tasks(body.value.afterId ?? "");
        case "listDatasetVersions":
          return {
            versions: [
              { dataset: { version: current, createdMs: 3n }, added: 1, tables: [Table.DAILY] },
              { dataset: { version: older, createdMs: 2n }, added: 2, tables: [Table.DAILY] },
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
const list = () => screen.findByRole("table", { name: "品种采集进度" });
const showKind = (name: "日线" | "分钟线") =>
  fireEvent.click(
    within(screen.getByRole("group", { name: "数据" })).getByRole("button", {
      name: new RegExp(`^${name}`),
    }),
  );
const openRebar = async () => {
  fireEvent.click(within(await list()).getByRole("button", { name: /^rb\s*螺纹钢/ }));
  await screen.findByRole("button", { name: "查看 rb2701" });
};

test("the summary shows freshness, acquisition defaults to canonical exchange order, and update brings all current", async () => {
  serve();
  view(context());
  const summary = await screen.findByRole("group", { name: "数据" });
  const [daily, minutes] = summary.querySelectorAll<HTMLElement>(".data-summary-item");
  expect(daily!.textContent).toBe("日线缺 2 个交易日");
  expect(daily!.title).toBe("数据到 2026-11-10 · 应到 2026-11-12 · 另有 3 个交易日来源无数据");
  expect(minutes!.textContent).toBe("分钟线");

  fireEvent.click(screen.getByRole("button", { name: "更新到最新" }));
  await screen.findByText("已提交 2 个更新任务");
  expect(request).toHaveBeenCalledWith({ case: "startUpdate", value: {} });
});

test("products with problems come first", async () => {
  serve();
  view(context());
  const table = await list();
  const names = within(table)
    .getAllByRole("rowheader")
    .map(cell => cell.textContent);
  expect(names).toEqual(["rb螺纹钢SHFE", "au沪金SHFE", "ag沪银SHFE", "zn沪锌SHFE", "已退市品种"]);
  const row = (name: RegExp) => within(table).getByRole("rowheader", { name }).closest("tr")!;
  expect(row(/^rb/).textContent).toContain("2 / 5");
});

test("a product opens on its listed contracts with history folded by delivery year", async () => {
  serve();
  view(context());
  await openRebar();
  const table = await list();
  expect(within(table).queryByRole("button", { name: "查看 rb2610" })).toBeNull();
  const history = within(table).getByRole("button", { name: "历史合约" }).closest("tr")!;
  expect(history.textContent).toContain("3 个");
  fireEvent.click(within(history).getByRole("button", { name: "历史合约" }));
  const year = (value: string) =>
    within(table)
      .getByRole("button", { name: `${value} 年交割` })
      .closest("tr")!;
  expect(year("2026").textContent).toContain("1 / 1");
  fireEvent.click(within(year("2026")).getByRole("button", { name: "2026 年交割" }));
  expect(within(table).getByRole("button", { name: "查看 rb2610" })).toBeTruthy();
});

test("the page shows one kind of data at a time, each contract's whole life by share of time", async () => {
  serve();
  view(context());
  await openRebar();
  const parts = (code: string) =>
    [
      ...screen
        .getByRole("button", { name: `查看 ${code}` })
        .closest("tr")!
        .querySelectorAll<HTMLElement>(".coverage-lane i"),
    ].map(item => item.dataset.part);
  expect(parts("rb2702")).toEqual(["future", "held", "gap"]);
  const rb2702Lane = screen
    .getByRole("button", { name: "查看 rb2702" })
    .closest("tr")!
    .querySelector<HTMLElement>(".coverage-lane")!;
  expect(rb2702Lane.title).toContain("2026-09-10–2026-09-11");
  fireEvent.click(screen.getByRole("button", { name: "历史合约" }));
  fireEvent.click(screen.getByRole("button", { name: "2025 年交割" }));
  const rb2510Lane = screen
    .getByRole("button", { name: "查看 rb2510" })
    .closest("tr")!
    .querySelector<HTMLElement>(".coverage-lane")!;
  expect(rb2510Lane.title).toContain("前段未采 4 天");
  fireEvent.click(screen.getByRole("button", { name: "2026 年交割" }));
  // A delisted contract held from listing to delisting fills the bar.
  const whole = screen
    .getByRole("button", { name: "查看 rb2610" })
    .closest("tr")!
    .querySelector<HTMLElement>(".coverage-lane i")!;
  expect(whole.dataset.part).toBe("held");
  expect(parseFloat(whole.style.left)).toBe(0);
  expect(parseFloat(whole.style.width)).toBe(100);

  showKind("分钟线");
  expect(parts("rb2701")).toEqual(["future", "held"]);
  expect(parts("rb2702")).toEqual(["future"]);
});

test("a contract is completed at once for the kind on show", async () => {
  serve();
  view(context());
  await openRebar();
  fireEvent.click(screen.getByRole("button", { name: "补全 rb2702" }));
  await waitFor(() => expect(submitted()).toHaveLength(1));
  expect(screen.queryByRole("dialog")).toBeNull();
  expect(submitted()[0]).toMatchObject({
    table: Table.DAILY,
    exchanges: ["SHFE"],
    refetch: false,
    firstDay: "2026-09-10",
    lastDay: "2026-09-11",
  });
  showKind("分钟线");
  fireEvent.click(screen.getByRole("button", { name: "采集 rb2702" }));
  await waitFor(() => expect(submitted()).toHaveLength(2));
  expect(submitted()[1]).toMatchObject({
    table: Table.MINUTES,
    instruments: ["SHFE/rb/2027-02"],
    refetch: false,
    firstDay: "",
    lastDay: "",
  });
});

test("going back to a year confirms how many contracts and from when", async () => {
  serve();
  view(context());
  await openRebar();
  fireEvent.click(screen.getByRole("button", { name: "历史合约" }));
  fireEvent.click(screen.getByRole("button", { name: "采集 SHFE/rb 2024" }));
  fireEvent.click(
    within(screen.getByRole("dialog", { name: "采集 rb 2024 年交割的 1 个合约" })).getByRole(
      "button",
      { name: "取消" },
    ),
  );
  fireEvent.click(screen.getByRole("button", { name: "采集全部 SHFE/rb" }));
  const dialog = screen.getByRole("dialog", { name: "采集 rb 2024–2025 年交割的 2 个合约" });
  expect(dialog.textContent).toContain("数据大约从 2023-10 开始");
  expect(dialog.textContent).toContain("SHFE 其他品种");
  expect(submitted()).toHaveLength(0);
  startInDialog();
  await waitFor(() => expect(submitted()).toHaveLength(1));
  expect(submitted()[0]).toMatchObject({
    exchanges: ["SHFE"],
    refetch: false,
    firstDay: "2023-10-16",
    lastDay: "2025-07-01",
  });
});

test("going back in minutes names every contract and the requests it takes", async () => {
  serve();
  view(context());
  await openRebar();
  showKind("分钟线");
  fireEvent.click(screen.getByRole("button", { name: "历史合约" }));
  fireEvent.click(screen.getByRole("button", { name: "采集全部 SHFE/rb" }));
  const dialog = screen.getByRole("dialog", { name: "采集 rb 2024–2026 年交割的 3 个合约" });
  expect(dialog.textContent).toContain("次请求");
  startInDialog();
  await waitFor(() => expect(submitted()).toHaveLength(1));
  expect(submitted()[0]).toMatchObject({
    table: Table.MINUTES,
    instruments: ["SHFE/rb/2024-10", "SHFE/rb/2025-10", "SHFE/rb/2026-10"],
    firstDay: "",
    lastDay: "",
  });
});

test("a contract an active task covers cannot be started again", async () => {
  serve({
    tasks: () => ({
      tasks: [
        create(AcquisitionTaskSchema, {
          request: {
            id: "acq-running",
            table: Table.DAILY,
            exchanges: ["CFFEX"],
            firstDay: "2026-09-01",
            lastDay: "2026-09-30",
          },
          state: TaskState.RUNNING,
        }),
        create(AcquisitionTaskSchema, {
          request: {
            id: "acq-minutes",
            table: Table.MINUTES,
            instruments: ["SHFE/rb/2027-02"],
          },
          state: TaskState.RUNNING,
        }),
      ],
      nextId: "",
    }),
  });
  view(context());
  await openRebar();
  expect(screen.getByRole("button", { name: "补全 rb2702" })).toBeTruthy();
  showKind("分钟线");
  await waitFor(() => expect(screen.queryByRole("button", { name: "采集 rb2702" })).toBeNull());
  expect(
    within(screen.getByRole("button", { name: "查看 rb2702" }).closest("tr")!).getByText("采集中"),
  ).toBeTruthy();
});

test("searching a contract code opens its product and shows that contract", async () => {
  serve();
  view(context());
  await list();
  fireEvent.change(screen.getByRole("searchbox"), { target: { value: "rb2610" } });
  expect(await screen.findByRole("button", { name: "查看 rb2610" })).toBeTruthy();
  expect(screen.queryByRole("button", { name: "查看 rb2701" })).toBeNull();
  expect(screen.queryByRole("rowheader", { name: /^ag/ })).toBeNull();
});

test("retired products fold at the bottom, confirm before acquiring all, and expand straight to years", async () => {
  serve();
  view(context());
  const table = await list();
  expect(within(table).queryByRole("rowheader", { name: /^pb/ })).toBeNull();
  const group = within(table).getByRole("button", { name: "已退市品种" });
  expect(group.closest("tr")!.textContent).toContain("1 个");

  fireEvent.click(group);
  expect(within(table).getByRole("rowheader", { name: /^pb/ }).textContent).toContain("沪铅");
  fireEvent.click(within(table).getByRole("button", { name: "采集全部 SHFE/pb" }));
  const dialog = await screen.findByRole("dialog", { name: "采集 pb 2023–2024 年交割的 2 个合约" });
  expect(dialog.textContent).toContain("数据大约从 2022-10 开始");
  expect(submitted()).toHaveLength(0);
  fireEvent.click(within(dialog).getByRole("button", { name: "取消" }));

  fireEvent.click(within(table).getByRole("button", { name: /^pb\s*沪铅/ }));
  expect(await within(table).findByRole("button", { name: "2024 年交割" })).toBeTruthy();
  expect(within(table).getByRole("button", { name: "2023 年交割" })).toBeTruthy();
  expect(within(table).queryByRole("button", { name: "历史合约" })).toBeNull();

  // Searching a retired product or its contract reveals it automatically.
  fireEvent.click(within(table).getByRole("button", { name: /^pb\s*沪铅/ }));
  fireEvent.click(group);
  expect(within(table).queryByRole("rowheader", { name: /^pb/ })).toBeNull();
  fireEvent.change(screen.getByRole("searchbox"), { target: { value: "pb2410" } });
  expect(await screen.findByRole("button", { name: "查看 pb2410" })).toBeTruthy();
});

test("a contract's chart opens over the list and closes on a second click or Escape", async () => {
  serve();
  view(context());
  await openRebar();
  const contract = screen.getByRole("button", { name: "查看 rb2701" });
  fireEvent.click(contract);
  expect(screen.getByRole("complementary", { name: "rb2701" })).toBeTruthy();
  expect(contract.getAttribute("aria-pressed")).toBe("true");
  fireEvent.click(contract);
  expect(screen.queryByRole("complementary")).toBeNull();
  fireEvent.click(contract);
  fireEvent.keyDown(document, { key: "Escape" });
  expect(screen.queryByRole("complementary")).toBeNull();
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
  await list();

  showKind("分钟线");
  fireEvent.click(screen.getByRole("button", { name: "补全 SHFE/rb" }));
  await waitFor(() => expect(submitted().map(item => item.table)).toEqual([Table.MINUTES]));
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
  await screen.findByRole("rowheader", { name: /^cu/ });
  expect(screen.queryByRole("button", { name: /^补全/ })).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "回到最新数据" }));
  await screen.findByRole("rowheader", { name: /^rb/ });
});

test("without an acquisition source nothing offers acquisition", async () => {
  serve({ tables: [] });
  view(context());
  await openRebar();
  expect(screen.queryByRole("button", { name: /^补全/ })).toBeNull();
  expect(screen.queryByRole("button", { name: "更新到最新" })).toBeNull();
});

test("a warehouse without a contract table offers no product to act on", async () => {
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables: [{ table: Table.DAILY, exchanges: ["SHFE"] }] };
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
  expect(await screen.findByText("暂无品种")).toBeTruthy();
  expect(screen.queryByRole("table", { name: "品种采集进度" })).toBeNull();
});

test("an unreachable warehouse shows a status card with address and settings entry", async () => {
  request.mockRejectedValue(new Error("warehouse offline"));
  const openSettings = vi.fn();
  view({ ...context(), openSettings });
  const alert = await screen.findByRole("alert");
  expect(within(alert).getByText("数据服务")).toBeTruthy();
  expect(within(alert).getByText("服务未连接")).toBeTruthy();
  expect(within(alert).getByText("market:7450")).toBeTruthy();
  expect(alert.textContent).toContain("warehouse offline");
  expect(within(alert).queryByRole("button", { name: "刷新" })).toBeNull();
  fireEvent.click(within(alert).getByRole("button", { name: "数据服务设置" }));
  expect(openSettings).toHaveBeenCalledWith("warehouse");
});

test("a kind link opens that kind", async () => {
  serve();
  const props = context({ kind: "minutes" });
  view(props);
  await list();
  expect(
    within(screen.getByRole("group", { name: "数据" }))
      .getByRole("button", { name: /^分钟线/ })
      .getAttribute("aria-pressed"),
  ).toBe("true");
  expect(props.navigate).toHaveBeenCalledWith("workspace.data-dataset", {});
});

test("initial dataset mount lists dataset versions only once", async () => {
  serve();
  view(context());
  await list();
  await waitFor(() =>
    expect(request.mock.calls.filter(([body]) => body.case === "listDatasetVersions")).toHaveLength(
      1,
    ),
  );
});

test("partial source-empty contracts stay acquirable while settled empty products offer no dead button", async () => {
  request.mockImplementation(async (body: { case: string }) => {
    switch (body.case) {
      case "getAcquisitionOptions":
        return { tables: [{ table: Table.DAILY, exchanges: ["SHFE"] }] };
      case "getDatasetSummary":
        return {
          dataset: { version: current, createdMs: 3n },
          tables: [
            {
              table: Table.DAILY,
              rows: 5n,
              firstDay: "2026-07-01",
              lastDay: "2026-11-12",
              expectedLastDay: "2026-11-12",
              missing: 1n,
            },
          ],
        };
      case "getDatasetCoverage":
        return create(DatasetCoverageSchema, {
          products: [
            {
              product: "SHFE/cu",
              contracts: 1,
              listed: 1,
              daily: { held: 0, listedComplete: 1 },
            },
            {
              product: "SHFE/rb",
              contracts: 1,
              listed: 1,
              daily: { held: 0, listedComplete: 0 },
            },
          ],
        });
      case "getProductCoverage":
        return create(ProductContractsSchema, {
          contracts: [
            {
              contract: "SHFE/rb/2027-03",
              listDay: "2026-10-01",
              delistDay: "2099-03-15",
              listed: true,
              daily: { days: 0, openDays: 10, emptyDays: 2 },
            },
          ],
        });
      case "listAcquisitionTasks":
        return { tasks: [], nextId: "" };
      case "listDatasetVersions":
        return { versions: [], nextVersion: "" };
      case "startUpdate":
        return { tasks: [] };
      default:
        return {};
    }
  });
  view(context());
  const table = await list();
  expect(within(table).queryByRole("button", { name: "采集 SHFE/cu" })).toBeNull();
  expect(within(table).getByRole("button", { name: "采集 SHFE/rb" })).toBeTruthy();

  fireEvent.click(within(table).getByRole("button", { name: /^rb\s*螺纹钢/ }));
  const row = (await screen.findByRole("button", { name: "查看 rb2703" })).closest("tr")!;
  expect(within(row).getByRole("button", { name: "采集 rb2703" })).toBeTruthy();
  expect(row.querySelector<HTMLElement>(".coverage-lane")!.title).toBe("未采集 · 来源无数据 2 天");

  fireEvent.click(screen.getByRole("button", { name: "更新到最新" }));
  await screen.findByText("各表末日已到最新；中间缺口请点击对应品种或合约的“补全”补齐");
});
