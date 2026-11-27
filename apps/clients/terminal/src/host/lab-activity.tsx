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
import { lab, onLabMutation } from "../bridge/lab";
import { RunState, TaskKind, type LabTask } from "../generated/asterion/lab_pb";
import { translate, type MessageValues } from "../i18n";
import { notify } from "./notifications";
import { useIntervalPoll } from "./polling";

const th = (key: string, values?: MessageValues) => translate("host", key, values);

export type LabActivityView = {
  runningProcesses: number;
  maxProcesses: number;
  queuedRuns: number;
  active: LabTask[];
  recent: LabTask[];
  totalActive: number;
  refresh: () => Promise<void>;
  cancelTask: (task: LabTask) => Promise<void>;
  cancelAll: () => Promise<void>;
};

const shortStrategy = (strategy: string) =>
  strategy.includes(".") ? strategy.split(".")[1]! : strategy;

const shortFactors = (strategy: string) =>
  strategy
    .split(" + ")
    .map(item => shortStrategy(item))
    .join(" + ");

export function labTaskTitle(task: LabTask) {
  const name = shortStrategy(task.strategy);
  switch (task.kind) {
    case TaskKind.BATCH:
      return th("批量回测");
    case TaskKind.SLIPPAGE_STRESS:
      return name ? th("滑点压测 {name}", { name }) : th("滑点压测");
    case TaskKind.SWEEP:
      return name ? th("扫描 {name}", { name }) : th("参数扫描");
    case TaskKind.ROLLING:
      return name ? th("滚动验证 {name}", { name }) : th("滚动验证");
    case TaskKind.FACTOR:
      if (task.strategy.includes(" + "))
        return th("合成因子 {name}", { name: shortFactors(task.strategy) });
      return name ? th("评价 {name}", { name }) : th("因子评价");
    case TaskKind.FACTOR_BATCH:
      return th("批量评价因子");
    default:
      return name ? th("回测 {name}", { name }) : th("回测");
  }
}

export function formatDuration(ms: number) {
  const sec = Math.max(0, Math.round(ms / 1000));
  if (sec < 60) return `${sec}s`;
  const min = Math.floor(sec / 60);
  const rem = sec % 60;
  if (min < 60) return rem > 0 ? `${min}m${rem}s` : `${min}m`;
  const hr = Math.floor(min / 60);
  const m = min % 60;
  return m > 0 ? `${hr}h${m}m` : `${hr}h`;
}

export function estimateRemainingMs(
  done: number,
  total: number,
  startedMs: number,
  nowMs: number,
): number | null {
  if (done <= 0 || total <= done || startedMs <= 0) return null;
  const elapsed = nowMs - startedMs;
  if (elapsed < 1000) return null;
  return Math.round((elapsed / done) * (total - done));
}

function sameTasks(a: readonly LabTask[], b: readonly LabTask[]) {
  if (a === b) return true;
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; ++i) {
    const x = a[i]!;
    const y = b[i]!;
    if (
      x.id !== y.id ||
      x.done !== y.done ||
      x.total !== y.total ||
      x.succeeded !== y.succeeded ||
      x.failed !== y.failed ||
      x.running !== y.running ||
      x.queued !== y.queued ||
      x.state !== y.state ||
      x.finishedMs !== y.finishedMs ||
      x.error !== y.error
    )
      return false;
  }
  return true;
}

const LabActivityContext = createContext<LabActivityView | null>(null);

// Tracks active and session-recent Lab background tasks; polls only while work is active.
export function LabActivityProvider({
  ready,
  currentModule,
  openRun,
  openSweep,
  openFactors,
  children,
}: {
  ready: boolean;
  currentModule: string;
  openRun: (id: string) => void;
  openSweep: (id: string) => void;
  openFactors?: (id?: string) => void;
  children: ReactNode;
}) {
  const [state, setState] = useState<{
    runningProcesses: number;
    maxProcesses: number;
    queuedRuns: number;
    active: LabTask[];
    recent: LabTask[];
  }>({
    runningProcesses: 0,
    maxProcesses: 0,
    queuedRuns: 0,
    active: [],
    recent: [],
  });
  const currentModuleRef = useRef(currentModule);
  currentModuleRef.current = currentModule;
  const openRunRef = useRef(openRun);
  openRunRef.current = openRun;
  const openSweepRef = useRef(openSweep);
  openSweepRef.current = openSweep;
  const openFactorsRef = useRef(openFactors);
  openFactorsRef.current = openFactors;

  const seenActive = useRef(new Set<string>());
  const notified = useRef(new Set<string>());
  const pending = useRef(false);
  const mounted = useRef(true);

  const read = useCallback(async () => {
    if (!ready || pending.current) return;
    pending.current = true;
    try {
      const reply = await lab({ case: "getActivity", value: {} });
      if (!mounted.current || !reply) return;
      const active = [...(reply.active ?? [])];
      const recent = [...(reply.recent ?? [])];
      setState(previous => {
        if (
          previous.runningProcesses === reply.runningProcesses &&
          previous.maxProcesses === reply.maxProcesses &&
          previous.queuedRuns === reply.queuedRuns &&
          sameTasks(previous.active, active) &&
          sameTasks(previous.recent, recent)
        )
          return previous;
        return {
          runningProcesses: reply.runningProcesses,
          maxProcesses: reply.maxProcesses,
          queuedRuns: reply.queuedRuns,
          active,
          recent,
        };
      });

      for (const task of active) seenActive.current.add(task.id);
      for (const task of recent) {
        if (!seenActive.current.has(task.id) || notified.current.has(task.id)) continue;
        seenActive.current.delete(task.id);
        notified.current.add(task.id);
        if (currentModuleRef.current === "research") continue;
        const title = labTaskTitle(task);
        const onClick =
          task.kind === TaskKind.BACKTEST
            ? () => openRunRef.current(task.id)
            : task.kind === TaskKind.FACTOR
              ? () =>
                  openFactorsRef.current?.(task.state === RunState.SUCCEEDED ? task.id : undefined)
              : task.kind === TaskKind.FACTOR_BATCH
                ? () => openFactorsRef.current?.()
                : () => openSweepRef.current(task.id);
        if (task.state === RunState.SUCCEEDED) {
          notify(
            task.total > 1
              ? th("{title} 已完成（{done} 组）", { title, done: task.total })
              : th("{title} 已完成", { title }),
            { namespace: "asterion.terminal.research", severity: "info", onClick },
          );
        } else if (task.state === RunState.FAILED) {
          notify(task.error ? `${title}: ${task.error}` : th("{title} 失败", { title }), {
            namespace: "asterion.terminal.research",
            severity: "error",
            onClick,
          });
        } else if (task.state === RunState.INTERRUPTED && task.failed > 0) {
          notify(
            th("{title} 已中断（完成 {done} / {total}）", {
              title,
              done: task.succeeded,
              total: task.total,
            }),
            { namespace: "asterion.terminal.research", severity: "warning", onClick },
          );
        }
      }
    } catch {
      // Lab availability is already reported by the host snapshot.
    } finally {
      pending.current = false;
    }
  }, [ready]);

  useEffect(() => {
    mounted.current = true;
    if (ready) void read();
    return () => {
      mounted.current = false;
    };
  }, [ready, read]);

  useEffect(() => {
    if (!ready) return;
    return onLabMutation(() => {
      void read();
    });
  }, [ready, read]);

  const hasLabActive = ready && state.active.length > 0;
  useIntervalPoll(() => void read(), hasLabActive ? 1500 : null, [read, hasLabActive]);

  const totalActive = state.active.length;
  useEffect(() => {
    if (totalActive === 0) return;
    const guard = (event: BeforeUnloadEvent) => {
      event.preventDefault();
      event.returnValue = "";
    };
    window.addEventListener("beforeunload", guard);
    return () => window.removeEventListener("beforeunload", guard);
  }, [totalActive]);

  const cancelOne = (task: LabTask) =>
    task.kind === TaskKind.BACKTEST
      ? lab({ case: "cancelRun", value: { id: task.id } })
      : task.kind === TaskKind.FACTOR || task.kind === TaskKind.FACTOR_BATCH
        ? lab({ case: "cancelFactor", value: { id: task.id } })
        : lab({ case: "cancelSweep", value: { id: task.id } });

  const cancelTask = useCallback(
    async (task: LabTask) => {
      await cancelOne(task);
      await read();
    },
    [read],
  );

  const cancelAll = useCallback(async () => {
    for (const task of state.active) await cancelOne(task);
    await read();
  }, [state.active, read]);

  const value = useMemo<LabActivityView>(
    () => ({
      ...state,
      totalActive,
      refresh: read,
      cancelTask,
      cancelAll,
    }),
    [state, totalActive, read, cancelTask, cancelAll],
  );
  return <LabActivityContext.Provider value={value}>{children}</LabActivityContext.Provider>;
}

export function useLabActivity(): LabActivityView | null {
  return useContext(LabActivityContext);
}
