import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { warehouseData } from "../bridge/warehouse-data";
import { Table, TaskState, type AcquisitionTask } from "../generated/asterion/warehouse_pb";
import { translate, type MessageValues } from "../i18n";
import {
  BackendError,
  asDisplayError,
  isDeselectedService,
  remoteError,
  type DisplayError,
} from "../i18n/errors";
import { notify, useErrorNotification } from "./notifications";
import { useIntervalPoll, useWorkspaceActive } from "./polling";

const th = (key: string, values?: MessageValues) => translate("host", key, values);
const batchOf = (id: string) =>
  /^(acq-[A-Za-z0-9]+|update-\d+)-(?:daily|minutes|settlement|price_ticks|contracts|calendar|warehouse_receipts|member_positions|inventory)$/.exec(
    id,
  )?.[1] ?? id;
const tableLabel: Partial<Record<Table, string>> = {
  [Table.DAILY]: "日线",
  [Table.MINUTES]: "分钟 K 线",
  [Table.CALENDAR]: "交易日历",
  [Table.CONTRACTS]: "合约表",
  [Table.SETTLEMENT]: "结算参数",
  [Table.PRICE_TICKS]: "历史报价单位",
  [Table.WAREHOUSE_RECEIPTS]: "仓单",
  [Table.MEMBER_POSITIONS]: "会员持仓",
  [Table.INVENTORY]: "库存周报",
};

// Everything but market bars follows them without being asked for.
export const automaticTable = (table: Table) => table !== Table.DAILY && table !== Table.MINUTES;
export const automaticTask = (task: AcquisitionTask) => automaticTable(task.request!.table);

export type WarehouseActivity = {
  done: bigint;
  total: bigint;
  running: boolean;
  heading: string;
  summary: string;
};

export type WarehouseView = {
  key: string;
  error?: DisplayError;
  diskFree?: bigint;
  activity: WarehouseActivity | null;
  tasks?: AcquisitionTask[];
  nextTaskId: string;
  refresh: () => Promise<void>;
};

function summarizeActive(tasks: readonly AcquisitionTask[]): WarehouseActivity | null {
  const active = tasks.filter(item => [TaskState.QUEUED, TaskState.RUNNING].includes(item.state));
  if (!active.length) return null;
  const groups = new Map<string, AcquisitionTask[]>();
  for (const item of active) {
    const key = automaticTask(item) ? "background" : batchOf(item.request?.id ?? "");
    const list = groups.get(key);
    if (list) list.push(item);
    else groups.set(key, [item]);
  }
  const batches = [...groups.values()];
  const current =
    batches.find(group => group.some(item => item.state === TaskState.RUNNING)) ??
    batches.find(group => !automaticTask(group[0]!)) ??
    batches[0]!;
  const running = current.some(item => item.state === TaskState.RUNNING);
  const done = current.reduce((sum, item) => sum + (item.itemsDone ?? 0n), 0n);
  const total = current.reduce((sum, item) => sum + (item.items ?? 0n), 0n);
  const requests = current.reduce((sum, item) => sum + (item.requestsSent ?? 0n), 0n);
  const startedMs = current.reduce((min, item) => {
    const at = item.startedMs ?? 0n;
    return at > 0n && (min === 0n || at < min) ? at : min;
  }, 0n);
  const elapsedMin = startedMs > 0n ? (Date.now() - Number(startedMs)) / 60_000 : 0;
  const ratePerMin =
    running && requests > 0n && elapsedMin >= 0.05 ? Math.round(Number(requests) / elapsedMin) : 0;
  const primary =
    current.find(item => item.request?.table === Table.MINUTES) ??
    current.find(item => item.request?.table === Table.DAILY) ??
    current.find(item => item.state === TaskState.RUNNING) ??
    current[0];
  const targets = [
    ...new Set(
      current.flatMap(item => [
        ...(item.request?.exchanges ?? []),
        ...(item.request?.instruments ?? []),
      ]),
    ),
  ].join(", ");
  const kind =
    primary?.request?.table && tableLabel[primary.request.table]
      ? th(tableLabel[primary.request.table]!)
      : th("数据");
  const heading = automaticTask(current[0]!)
    ? `${th("后台更新")} · ${kind}`
    : `${th(running ? "采集中" : "等待")} · ${kind}${targets ? ` ${targets}` : ""}`;
  const parts = [
    heading,
    total > 0n && th("{done} / {total} 个分区", { done: done.toString(), total: total.toString() }),
    requests > 0n && th("{count} 次请求", { count: requests.toString() }),
    ratePerMin > 0 && th("{rate} 次请求/分钟", { rate: ratePerMin }),
    batches.length > 1 && th("另有 {count} 个任务等待", { count: batches.length - 1 }),
  ];
  return {
    done,
    total,
    running,
    heading,
    summary: parts.filter(Boolean).join(" · "),
  };
}

function sameTasks(
  a: readonly AcquisitionTask[] | undefined,
  b: readonly AcquisitionTask[] | undefined,
) {
  if (a === b) return true;
  if (!a || !b || a.length !== b.length) return false;
  for (let i = 0; i < a.length; ++i) {
    const x = a[i]!;
    const y = b[i]!;
    if (
      x.request?.id !== y.request?.id ||
      x.state !== y.state ||
      x.itemsDone !== y.itemsDone ||
      x.items !== y.items ||
      x.requestsSent !== y.requestsSent ||
      x.finishedMs !== y.finishedMs ||
      x.error?.message !== y.error?.message
    )
      return false;
  }
  return true;
}

const Warehouse = createContext<WarehouseView | null>(null);

// Single host poll for Warehouse status, active task progress and newly failed tasks.
export function WarehouseProvider({
  endpoint,
  children,
}: {
  endpoint: string;
  children: ReactNode;
}) {
  const [state, setState] = useState<{
    key: string;
    error?: DisplayError;
    diskFree?: bigint;
    activity: WarehouseActivity | null;
    tasks?: AcquisitionTask[];
    nextTaskId: string;
  }>({ key: endpoint, activity: null, nextTaskId: "" });
  const current = useMemo(
    () =>
      endpoint && state.key === endpoint
        ? state
        : { key: endpoint, activity: null, nextTaskId: "" },
    [endpoint, state],
  );
  useErrorNotification(current.error, "asterion.terminal.data-workbench");
  const seen = useRef<{ key: string; ids: Set<string> | null }>({ key: endpoint, ids: null });
  const readRef = useRef<() => Promise<void>>(async () => {});

  useEffect(() => {
    if (!endpoint) {
      readRef.current = async () => {};
      return;
    }
    if (seen.current.key !== endpoint) seen.current = { key: endpoint, ids: null };
    let active = true;
    let pending = false;
    const read = async () => {
      if (pending) return;
      pending = true;
      try {
        const status = await warehouseData({ case: "getStatus", value: {} });
        if (!active) return;
        // Leave the initial "connecting" state right away; steady-state polls merge both replies below.
        setState(previous =>
          previous.key === endpoint && previous.error === ""
            ? previous
            : {
                key: endpoint,
                error: "",
                diskFree: status?.diskFreeBytes,
                activity: previous.key === endpoint ? previous.activity : null,
                tasks: previous.key === endpoint ? previous.tasks : undefined,
                nextTaskId: previous.key === endpoint ? previous.nextTaskId : "",
              },
        );
        const page = await warehouseData({ case: "listAcquisitionTasks", value: { afterId: "" } });
        if (!active || !Array.isArray(page?.tasks)) return;
        const nextActivity = summarizeActive(page.tasks);
        const nextTaskId = page.nextId ?? "";
        setState(previous => {
          if (
            previous.key === endpoint &&
            previous.error === "" &&
            previous.diskFree === status?.diskFreeBytes &&
            previous.nextTaskId === nextTaskId &&
            previous.activity?.summary === nextActivity?.summary &&
            sameTasks(previous.tasks, page.tasks)
          )
            return previous;
          return {
            key: endpoint,
            error: "",
            diskFree: status?.diskFreeBytes,
            activity: nextActivity,
            tasks: page.tasks,
            nextTaskId,
          };
        });
        const failed = page.tasks.filter(task =>
          [TaskState.FAILED, TaskState.INTERRUPTED].includes(task.state),
        );
        if (!seen.current.ids) {
          seen.current.ids = new Set(failed.map(task => task.request!.id));
          return;
        }
        for (const task of [...failed].reverse()) {
          const id = task.request!.id;
          if (seen.current.ids.has(id)) continue;
          seen.current.ids.add(id);
          notify(task.error ? remoteError(task.error) : new BackendError("operation_failed", id), {
            namespace: "asterion.terminal.data-workbench",
          });
        }
      } catch (failure) {
        if (active && !isDeselectedService(failure)) {
          setState({
            key: endpoint,
            error: asDisplayError(failure),
            activity: null,
            nextTaskId: "",
          });
        }
      } finally {
        pending = false;
      }
    };
    readRef.current = read;
    void read();
    return () => {
      active = false;
    };
  }, [endpoint]);

  const activeBusy = Boolean(endpoint && current.activity && !current.error);
  useIntervalPoll(() => void readRef.current(), endpoint ? (activeBusy ? 1500 : 5000) : null, [
    endpoint,
    activeBusy,
  ]);

  const refresh = useCallback(() => readRef.current(), []);
  const value = useMemo(() => ({ ...current, refresh }), [current, refresh]);
  return <Warehouse.Provider value={value}>{children}</Warehouse.Provider>;
}

export function useWarehouse(): WarehouseView | null {
  return useContext(Warehouse);
}

// Reads a Warehouse view now and, unless `once`, every 3 s without overlapping slow reads.
export function useWarehousePoll<T>(
  load: () => Promise<T>,
  deps: readonly unknown[],
  once = false,
) {
  const workspaceActive = useWorkspaceActive();
  const [value, setValue] = useState<T>();
  const [error, setError] = useState<DisplayError>("");
  const inFlight = useRef(0);
  const latest = useRef(0);
  const mounted = useRef(true);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const read = useCallback(load, deps);
  const run = useCallback(
    async (background: boolean) => {
      if (background && inFlight.current > 0) return;
      const seq = ++latest.current;
      inFlight.current += 1;
      try {
        const next = await read();
        if (mounted.current && seq === latest.current) {
          setValue(next);
          setError("");
        }
      } catch (failure) {
        if (mounted.current && seq === latest.current) setError(asDisplayError(failure));
      } finally {
        inFlight.current -= 1;
      }
    },
    [read],
  );
  const refresh = useCallback(() => run(false), [run]);
  useEffect(() => {
    mounted.current = true;
    void run(false);
    return () => {
      mounted.current = false;
    };
  }, [run]);
  const wasActive = useRef(workspaceActive);
  useEffect(() => {
    if (!wasActive.current && workspaceActive && !once) void run(true);
    wasActive.current = workspaceActive;
  }, [workspaceActive, once, run]);
  useIntervalPoll(() => void run(true), once ? null : 3000, [run, once]);
  return { value, error, refresh };
}
