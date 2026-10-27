import { ResearchVariablePicker } from "./ResearchVariable";
import type { ResourceObject, ResearchVariableDefinition } from "../../src/bridge/client";
import { taskOwner } from "../../src/bridge/client";
import { ResearchExport } from "./ResearchExport";
import { useEffect, useRef, useState } from "react";
import { useWorkspaceDraft, type TerminalContext } from "../contract";
import { BarFactorForm, TableFactorForm } from "./FactorForm";
import { FactorResults } from "./FactorResults";
import { FactorComparison } from "./FactorComparison";
import { FactorReuse } from "./FactorReuse";
import { ResearchPage, t, TaskRecords, useRun, variableUnit } from "./shared";

const inputs = { bars: "K 线", tables: "全市场数据表" };

/**
 * Factor evaluation. The series come from datasets of contracts' bars or from
 * instruments of the market-wide daily table; the task, its parameters and
 * its result are the same either way.
 */
export function Factor({
  snapshot,
  busy,
  trade,
  query,
  navigate,
  workspaceParams,
}: TerminalContext) {
  const taskService = snapshot?.research_service;
  const [variable, setVariable] = useWorkspaceDraft<ResourceObject | null>("factor-variable", null);
  const [definition, setDefinition] = useState<ResearchVariableDefinition | null>(null);
  const variableHint = workspaceParams?.variable_id
    ? JSON.stringify([
        workspaceParams.variable_id,
        workspaceParams.variable_version,
        workspaceParams.variable_node,
        workspaceParams.variable_service,
      ])
    : "";
  const [seenVariable, setSeenVariable] = useWorkspaceDraft("factor-variable-hint", "");
  useEffect(() => {
    if (!variableHint || variableHint === seenVariable) return;
    const [id, version, node, service] = JSON.parse(variableHint) as string[];
    setDefinition(null);
    setVariable({ id, version, node, service, domain: "research", kind: "research_variable" });
    setSeenVariable(variableHint);
  }, [variableHint, seenVariable, setSeenVariable, setVariable]);
  const variableReady =
    (!variable ||
      Boolean(
        definition &&
        definition.object.id === variable.id &&
        definition.object.version === variable.version &&
        definition.object.node === variable.node &&
        definition.object.service === variable.service,
      )) &&
    (!variableHint || variableHint === seenVariable);

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
    <ResearchPage taskService={taskService} error={error}>
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
          <ResearchVariablePicker
            query={query}
            snapshot={snapshot}
            reference={variable}
            onChange={setVariable}
            onResolved={setDefinition}
          />
          {variable && definition && variableReady && (
            <section aria-label={t("原变量定义")}>
              <p>
                {definition.name} · {definition.purpose}
              </p>
              <pre>{definition.expression}</pre>
              <p>
                {variableUnit(definition.price_power)} · {t("实际预热观察数")}:{" "}
                {definition.required_history}
              </p>
            </section>
          )}
          {input === "tables" ? (
            <TableFactorForm
              query={query}
              snapshot={snapshot}
              busy={busy}
              navigate={navigate}
              run={run}
              variableDefinition={variable ? definition : null}
              variableReady={variableReady}
            />
          ) : (
            <BarFactorForm
              query={query}
              snapshot={snapshot}
              busy={busy}
              trade={trade}
              navigate={navigate}
              run={run}
              variableDefinition={variable ? definition : null}
              variableReady={variableReady}
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
            onResult={task =>
              void run("task.result", {
                owner: taskOwner(snapshot?.research_service, "research"),
                id: task.id,
              }).then(ok => {
                setShown(ok);
              })
            }
          />
          <FactorComparison snapshot={snapshot} busy={busy} query={query} />
          <div ref={results}>
            {snapshot?.task_result?.kind === "factor" && (
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
