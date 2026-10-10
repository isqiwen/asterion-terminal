import { useEffect, useRef, useState } from "react";
import { useWorkspaceDraft, type TerminalContext } from "../contract";
import { BarFactorForm, DailyFactorForm } from "./FactorForm";
import { FactorResults } from "./FactorResults";
import { ResearchPage, t, TaskRecords, useRun } from "./shared";

const inputs = { bars: "K 线", daily: "日线" };

/**
 * Momentum factor evaluation. The series comes either from a dataset of one
 * contract's bars or from a daily version published in the archive; the task,
 * its parameters and its result are the same either way.
 */
export function Factor({
  snapshot,
  busy,
  trade,
  query,
  navigate,
  workspacePage,
  workspaceParams,
}: TerminalContext) {
  const taskService = snapshot?.task_service;
  const { error, run } = useRun(trade);
  const [input, setInput] = useWorkspaceDraft<keyof typeof inputs>("factor-input", "bars");
  // A daily version handed over from the data page selects the daily input once.
  const handed =
    workspacePage === "factor" && workspaceParams?.source_dataset_id
      ? JSON.stringify([workspaceParams.source_dataset_id, workspaceParams.connection_id])
      : "";
  const [received, setReceived] = useWorkspaceDraft("factor-source-request", "");
  useEffect(() => {
    if (handed && received !== handed) {
      setReceived(handed);
      setInput("daily");
    }
  }, [handed, received, setReceived, setInput]);
  // A result opens below the records; asking for one scrolls to it.
  const results = useRef<HTMLDivElement>(null);
  const [shown, setShown] = useState(false);
  const shownResult = snapshot?.task_result?.id;
  useEffect(() => {
    if (!shown) return;
    results.current?.scrollIntoView({ block: "start" });
    setShown(false);
  }, [shown, shownResult]);
  return (
    <ResearchPage title={t("动量因子")} taskService={taskService} error={error}>
      <div className="research-layout">
        <section className="research-config" aria-label={t("因子设置")}>
          <nav className="research-inputs" aria-label={t("因子数据")}>
            {Object.entries(inputs).map(([id, label]) => (
              <button
                key={id}
                aria-pressed={input === id}
                onClick={() => setInput(id as keyof typeof inputs)}
              >
                {t(label)}
              </button>
            ))}
          </nav>
          {input === "daily" ? (
            <DailyFactorForm
              query={query}
              snapshot={snapshot}
              busy={busy}
              navigate={navigate}
              run={run}
              workspaceParams={workspaceParams}
            />
          ) : (
            <BarFactorForm
              query={query}
              snapshot={snapshot}
              busy={busy}
              trade={trade}
              navigate={navigate}
              run={run}
            />
          )}
        </section>
        <div className="research-main">
          <TaskRecords
            heading={t("分析记录")}
            taskService={taskService}
            busy={busy}
            trade={trade}
            run={run}
            listed={task => task.kind === "factor"}
            onResult={task => void run("task.result", { id: task.id }).then(setShown)}
          />
          <div ref={results}>
            {snapshot?.task_result?.kind === "factor" && (
              <FactorResults key={snapshot.task_result.id} evidence={snapshot.task_result} />
            )}
          </div>
        </div>
      </div>
    </ResearchPage>
  );
}
