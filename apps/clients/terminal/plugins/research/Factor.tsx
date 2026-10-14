import { useEffect, useRef, useState } from "react";
import { useWorkspaceDraft, type TerminalContext } from "../contract";
import { BarFactorForm, TableFactorForm } from "./FactorForm";
import { FactorResults } from "./FactorResults";
import { ResearchPage, t, TaskRecords, useRun } from "./shared";

const inputs = { bars: "K 线", tables: "全市场数据表" };

/**
 * Factor evaluation. The series come from datasets of contracts' bars or from
 * instruments of the market-wide daily table; the task, its parameters and
 * its result are the same either way.
 */
export function Factor({ snapshot, busy, trade, query, navigate }: TerminalContext) {
  const taskService = snapshot?.task_service;
  const { error, run } = useRun(trade);
  const [input, setInput] = useWorkspaceDraft<keyof typeof inputs>("factor-input", "bars");
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
    <ResearchPage title={t("因子分析")} taskService={taskService} error={error}>
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
          {input === "tables" ? (
            <TableFactorForm
              query={query}
              snapshot={snapshot}
              busy={busy}
              navigate={navigate}
              run={run}
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
