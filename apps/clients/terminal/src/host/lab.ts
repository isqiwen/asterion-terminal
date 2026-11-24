import { useCallback, useEffect, useState } from "react";
import { lab } from "../bridge/lab";
import { RunState, type RunSummary } from "../generated/asterion/lab_pb";
import { notify } from "./notifications";
import { useIntervalPoll, useSequentialPoll } from "./polling";

export type LabActiveReportTarget = { kind: "run" | "sweep"; id: string };

export type LabSweepState = {
  id: string;
  runs: RunSummary[];
  selected: string;
  rolling?: RunSummary;
  active: boolean;
};

// Polls a running backtest or parameter sweep sequentially every 1.5 s until it finishes.
export function useLabActiveReport(report: LabActiveReportTarget | null, onComplete: () => void) {
  const [activeRun, setActiveRun] = useState<RunSummary | null>(null);
  const [activeSweep, setActiveSweep] = useState<LabSweepState | null>(null);
  useEffect(() => {
    if (!report) {
      setActiveRun(null);
      setActiveSweep(null);
    }
  }, [report]);
  const readOnce = useCallback(
    async (isActive: () => boolean) => {
      if (!report) return false;
      if (report.kind === "run") {
        const summary = await lab({ case: "getRunSummary", value: { id: report.id } });
        if (!isActive()) return false;
        setActiveRun(summary);
        return summary.state === RunState.RUNNING;
      }
      const reply = await lab({ case: "getSweep", value: { id: report.id } });
      if (!isActive()) return false;
      setActiveSweep({
        id: report.id,
        runs: [...reply.runs],
        selected: reply.selectedId,
        rolling: reply.rolling,
        active: reply.active,
      });
      return reply.active;
    },
    [report],
  );
  useSequentialPoll(
    async isActive => {
      try {
        const stillRunning = await readOnce(isActive);
        if (!stillRunning && isActive()) onComplete();
        return stillRunning;
      } catch (failure) {
        if (isActive()) notify(failure, { namespace: "asterion.terminal.research" });
        return false;
      }
    },
    1500,
    Boolean(report),
    [readOnce, onComplete],
  );
  const refresh = useCallback(async () => {
    await readOnce(() => true);
  }, [readOnce]);
  return { activeRun, activeSweep, refresh };
}

// Polls Lab run listings every 2 s while any listed run or sweep is still running.
export function useLabRunningPoll(readRuns: () => Promise<void>, running: boolean) {
  useIntervalPoll(() => void readRuns(), running ? 2000 : null, [readRuns, running]);
}
