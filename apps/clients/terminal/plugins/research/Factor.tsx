import { ResearchExport } from "./ResearchExport";
import { useEffect, useRef, useState } from "react";
import { useWorkspaceDraft, type TerminalContext } from "../contract";
import { BarFactorForm, TableFactorForm } from "./FactorForm";
import { FactorResults } from "./FactorResults";
import { FactorComparison } from "./FactorComparison";
import { FactorReuse } from "./FactorReuse";
import { ResearchPage, t, TaskRecords, useRun, requiresNewFactorTask } from "./shared";

const inputs = { bars: "K 线", tables: "全市场数据表" };

/**
 * Factor evaluation. The series come from datasets of contracts' bars or from
 * instruments of the market-wide daily table; the task, its parameters and
 * its result are the same either way.
 */
export function Factor({ snapshot, busy, trade, query, navigate }: TerminalContext) {
  const taskService = snapshot?.task_service;
  const { error, setError, run } = useRun(trade);
  const diagnostic = error instanceof Error ? error.message : error;
  const incompatible = requiresNewFactorTask(diagnostic);
  const [recreating, setRecreating] = useState(false);
  const config = useRef<HTMLElement>(null);
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
      {incompatible && (
        <section aria-label={t("重新创建实验")}>
          <p>
            {t(
              "旧任务和固定输入文件已保留，当前版本不能读取。请重新选择已入库数据的快照、范围和参数后创建新任务；新任务不是旧实验的恢复，也不保证同一数据版本。不会迁移、覆盖或删除旧文件。",
            )}
          </p>
          <button
            onClick={() => {
              setInput("tables");
              setRecreating(true);
              setError("");
              config.current?.scrollIntoView({ block: "start" });
            }}
          >
            {t("重新选择日线输入")}
          </button>
        </section>
      )}
      <div className="research-layout">
        <section ref={config} className="research-config" aria-label={t("因子设置")}>
          {recreating && (
            <p className="subtle">{t("请核对新任务的快照与参数；原实验保留不变。")}</p>
          )}
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
            query={query}
            heading={t("分析记录")}
            taskService={taskService}
            busy={busy}
            trade={trade}
            run={run}
            listed={task => task.kind === "factor"}
            onRecreate={() => {
              setInput("tables");
              setRecreating(true);
              setError("");
              config.current?.scrollIntoView({ block: "start" });
            }}
            onResult={task =>
              void run("task.result", { id: task.id }).then(ok => {
                setShown(ok);
                if (ok) setRecreating(false);
              })
            }
          />
          <FactorComparison snapshot={snapshot} busy={busy} query={query} />
          <div ref={results}>
            {!recreating && !incompatible && snapshot?.task_result?.kind === "factor" && (
              <>
                <FactorResults key={snapshot.task_result.id} evidence={snapshot.task_result} />
                <ResearchExport
                  key={`export-${snapshot.task_result.id}`}
                  id={snapshot.task_result.id}
                  kind="factor"
                  busy={busy}
                  query={query}
                />
                <FactorReuse
                  key={`reuse-${snapshot.task_result.id}`}
                  evidence={snapshot.task_result}
                  snapshot={snapshot}
                  busy={busy}
                  query={query}
                  run={run}
                />
              </>
            )}
          </div>
        </div>
      </div>
    </ResearchPage>
  );
}
