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
import { useIntervalPoll } from "./polling";

const th = (key: string, values?: MessageValues) => translate("host", key, values);
const batchOf = (id: string) =>
  /^(acq-[A-Za-z0-9]+|update-\d+)-(?:daily|minutes|settlement|price_ticks|contracts|calendar|warehouse_receipts|member_positions|inventory)$/.exec(
    id,
  )?.[1] ?? id;
const tableLabel: Partial<Record<Table, string>> = {
  [Table.DAILY]: "日线",
  [Table.MINUTES]: "分钟 K 线",
};

// Everything but market bars follows them automatically; only a stopped task concerns the user.
export const backgroundTask = (task: AcquisitionTask) =>
  ![Table.DAILY, Table.MINUTES].includes(task.request!.table) &&
  ![TaskState.FAILED, TaskState.INTERRUPTED, TaskState.CANCELLED].includes(task.state);

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
  const active = tasks.filter(
    item => !backgroundTask(item) && [TaskState.QUEUED, TaskState.RUNNING].includes(item.state),
  );
  if (!active.length) return null;
  const groups = new Map<string, AcquisitionTask[]>();
  for (const item of active) {
    const key = batchOf(item.request?.id ?? "");
    const list = groups.get(key);
    if (list) list.push(item);
    else groups.set(key, [item]);
  }
  const batches = [...groups.values()];
  const current =
    batches.find(group => group.some(item => item.state === TaskState.RUNNING)) ?? batches[0]!;
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
  const heading = `${th(running ? "采集中" : "等待")} · ${kind}${targets ? ` ${targets}` : ""}`;
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
        setState(previous => ({
          key: endpoint,
          error: "",
          diskFree: status?.diskFreeBytes,
          activity: previous.key === endpoint ? previous.activity : null,
          tasks: previous.key === endpoint ? previous.tasks : undefined,
          nextTaskId: previous.key === endpoint ? previous.nextTaskId : "",
        }));
        const page = await warehouseData({ case: "listAcquisitionTasks", value: { afterId: "" } });
        if (!active || !Array.isArray(page?.tasks)) return;
        const nextActivity = summarizeActive(page.tasks);
        setState(previous => ({
          key: endpoint,
          error: "",
          diskFree: previous.key === endpoint ? previous.diskFree : status?.diskFreeBytes,
          activity: nextActivity,
          tasks: page.tasks,
          nextTaskId: page.nextId ?? "",
        }));
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
  useIntervalPoll(() => void run(true), once ? null : 3000, [run, once]);
  return { value, error, refresh };
}
