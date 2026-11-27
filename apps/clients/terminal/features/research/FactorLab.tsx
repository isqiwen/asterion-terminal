import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type KeyboardEvent,
  type ReactNode,
} from "react";
import { lab } from "../../src/bridge/lab";
import { RunState, TaskKind, type FactorResult } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { useLabActivity } from "../../src/host/lab-activity";
import { notify, useErrorNotification } from "../../src/host/notifications";
import { BackendError, type DisplayError, asDisplayError } from "../../src/i18n/errors";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { dataGapParams, parseDataGap } from "./data-gap";
import { factorTurnover } from "./research-analytics";
import {
  defaultText,
  factorRequest,
  factorScopeOf,
  identity,
  onFactorScope,
  recordName,
  recordTime,
  savedFactorForm,
  type FactorForm,
} from "./research-lab";
import { deleteAnnotations, symbolsOf } from "./research-storage";
import { factorStrategyStem, factorStrategyTemplate } from "./research-templates";
import { FactorComparison, FactorRecords, factorPeriodTag } from "./FactorRecords";
import { FactorResultView } from "./FactorResultView";
import { FactorScopeBar, useFactorConditions } from "./FactorScope";
import {
  ConfigFold,
  DeleteRecord,
  filterModuleItems,
  LabCentre,
  ModuleErrors,
  type ModuleItem,
  NewModule,
  Section,
  useAnnotations,
  useConfigFold,
  useLibrarySort,
  useModuleMenu,
  useModules,
  useSource,
} from "./LabParts";
import { metric, shortTime, signed } from "./result-parts";
import { datePlaceholders, useStoredCatalog } from "./SymbolPicker";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// The factors it evaluated and when.
const resultName = (item: FactorResult) => {
  const names = item.factorNames.length ? item.factorNames : [item.evaluation?.factor ?? ""];
  return `${names.map(name => name.split(".")[1] ?? name).join(" + ")} ${recordTime(item.id)}`;
};
const isCompositeResult = (item: FactorResult) =>
  item.factorNames.length >= 2 || (item.evaluation?.extraFactors.length ?? 0) > 0;
const compositeName = (item: FactorResult) =>
  item.factorNames.length
    ? item.factorNames.map(name => name.split(".")[1] ?? name).join(" + ")
    : recordName(item.id);

// The factor view: every factor ranked on one set of evaluation settings, and
// one factor's evaluation and code with its parameters beside them.
export function FactorLab(context: TerminalContext) {
  const repository = context.snapshot?.research_directory ?? "";
  const { modules, read, forget } = useModules(repository);
  const [chosen, setChosen] = useWorkspaceDraft(`factor:${repository}`, "");
  const [report, setReport] = useWorkspaceDraft("factor-report", "");
  const [form, setForm] = useWorkspaceDraft<FactorForm>("factor-form", savedFactorForm);
  const conditions = useFactorConditions(setForm);
  const scopeText = JSON.stringify(factorScopeOf(form));
  const shownScope = useRef(scopeText);
  const [view, setView] = useWorkspaceDraft<"library" | "detail">("factor-view", "library");
  const [libraryTab, setLibraryTab] = useWorkspaceDraft<"library" | "records">(
    "factor-library-tab",
    "library",
  );
  const [tab, setTab] = useWorkspaceDraft<"result" | "code" | "split">("factor-tab", "code");
  const [search, setSearch] = useWorkspaceDraft("modules-search:factors", "");
  const [recordsSearch, setRecordsSearch] = useWorkspaceDraft("factor-records-search", "");
  const [comparing, setComparing] = useState<string[]>([]);
  const [onlyScopeRecords, setOnlyScopeRecords] = useState(false);
  const annotations = useAnnotations();
  const [submitting, setSubmitting] = useState(false);
  const [cancellingId, setCancellingId] = useState("");
  const trackedSingleRef = useRef<{ id: string; factorId: string } | null>(null);
  const trackedCompositeRef = useRef("");
  const trackedBatchRef = useRef("");
  const seenFactorActiveRef = useRef(new Set<string>());
  const handledRecentRef = useRef(new Set<string>());
  const lastBatchDoneRef = useRef<Record<string, number>>({});
  const { error, attempt, fail, clear } = useActionError();
  const source = useSource(repository, "factors");
  const [saved, setSaved] = useState<string[]>([]);
  const [selected, setSelected] = useState<string[]>([]);
  const [generating, setGenerating] = useState<FactorResult | null>(null);

  const library = view === "library";
  // An evaluation takes a while: its report opens only if the page is still where it began.
  const openPage = useRef("");
  useEffect(() => {
    openPage.current = library ? "" : chosen;
  });
  const factor = modules?.factors.find(item => item.id === chosen);
  const users = (modules?.strategies ?? []).filter(item =>
    item.factorModules.includes(chosen.split(".")[0]),
  );
  // Kept evaluations, newest first.
  const [kept, setKept] = useState<FactorResult[]>([]);
  const readKept = useCallback(async () => {
    const results = (await lab({ case: "listFactorResults", value: {} })).results;
    setKept(results);
    return results;
  }, []);
  useEffect(
    () =>
      void readKept().catch(failure =>
        notify(failure, { namespace: "asterion.terminal.research" }),
      ),
    [readKept],
  );
  const isMinutes = Boolean(form.minutes && form.minutes !== "0");
  const catalog = useStoredCatalog(context, isMinutes, true);
  const enteredSymbols = symbolsOf(form.symbols);
  const [scopeBegin, scopeEnd] = datePlaceholders(catalog, enteredSymbols, isMinutes);
  const inScope = enteredSymbols.length
    ? (result: FactorResult) => onFactorScope(form, result, scopeBegin, scopeEnd)
    : undefined;
  const own = (id: string) =>
    kept.filter(item => item.evaluation?.factor === id && !isCompositeResult(item));
  // The evaluation a factor ranks by: its latest on the current settings, or else its latest.
  const latest = (id: string) => {
    const results = own(id);
    return results.find(item => !inScope || inScope(item)) ?? results[0];
  };
  const composites = kept.filter(isCompositeResult);
  const shownResult = kept.find(item => item.id === report);
  const evaluations = kept.filter(item => item.evaluation?.factor === chosen);
  const items: ModuleItem[] = (modules?.factors ?? []).map(item => {
    const [module, name] = item.id.split(".");
    return {
      id: item.id,
      name,
      module,
      description: item.description.replace(/`/g, ""),
      inputs: item.inputs,
    };
  });
  const change = (patch: Partial<FactorForm>) => setForm(previous => ({ ...previous, ...patch }));
  const choose = (id: string) => {
    const entry = modules!.factors.find(item => item.id === id)!;
    const result = latest(id);
    setChosen(id);
    setReport(result?.id ?? "");
    setTab(result ? "result" : "code");
    setView("detail");
    source.open(`factors/${id.split(".")[0]}.py`);
    if (id !== chosen) {
      change({ params: Object.fromEntries(entry.parameters.map(p => [p.name, defaultText(p)])) });
    }
  };
  const openCode = (path: string, id = "") => {
    setChosen(id);
    setReport("");
    setTab("code");
    setView("detail");
    source.open(path);
  };
  const openLibrary = () => {
    setReport("");
    setComparing([]);
    setView("library");
  };
  const openRecord = (item: FactorResult) => {
    setComparing([]);
    const target = item.evaluation?.factor ?? "";
    if (!isCompositeResult(item) && target && modules?.factors.some(f => f.id === target)) {
      setChosen(target);
      setReport(item.id);
      setTab("result");
      setView("detail");
      source.open(`factors/${target.split(".")[0]}.py`);
      return;
    }
    setReport(item.id);
    setView("library");
  };
  const labActivity = useLabActivity();
  const activeFactorTasks = (labActivity?.active ?? []).filter(
    item => item.kind === TaskKind.FACTOR,
  );
  const activeBatchTask =
    (labActivity?.active ?? []).find(item => item.kind === TaskKind.FACTOR_BATCH) ?? null;
  const activeSingleTask =
    !library && factor
      ? (activeFactorTasks.find(
          item => item.id === trackedSingleRef.current?.id || item.strategy === factor.id,
        ) ?? null)
      : null;
  const activeCompositeTask =
    activeFactorTasks.find(
      item => item.id === trackedCompositeRef.current || item.strategy.includes(" + "),
    ) ?? null;

  useEffect(() => {
    clear();
  }, [view, chosen, form, clear]);

  useEffect(() => {
    for (const task of labActivity?.active ?? []) {
      if (task.kind !== TaskKind.FACTOR && task.kind !== TaskKind.FACTOR_BATCH) continue;
      seenFactorActiveRef.current.add(task.id);
      if (task.kind === TaskKind.FACTOR_BATCH) {
        const previous = lastBatchDoneRef.current[task.id] ?? 0;
        if (task.done > previous) {
          lastBatchDoneRef.current[task.id] = task.done;
          void readKept();
        }
      }
    }
    for (const task of labActivity?.recent ?? []) {
      if (task.kind !== TaskKind.FACTOR && task.kind !== TaskKind.FACTOR_BATCH) continue;
      const isSingle = trackedSingleRef.current?.id === task.id;
      const isComposite = trackedCompositeRef.current === task.id;
      const isBatch = trackedBatchRef.current === task.id;
      const wasSeen = seenFactorActiveRef.current.has(task.id);
      if (!isSingle && !isComposite && !isBatch && !wasSeen) continue;
      if (handledRecentRef.current.has(task.id)) continue;
      handledRecentRef.current.add(task.id);
      seenFactorActiveRef.current.delete(task.id);

      if (isSingle) {
        const targetFactor = trackedSingleRef.current!.factorId;
        trackedSingleRef.current = null;
        if (task.state === RunState.SUCCEEDED) {
          void readKept();
          if (openPage.current === targetFactor) {
            setReport(task.id);
            setTab(previous => (previous === "split" ? "split" : "result"));
          }
        } else if (task.state === RunState.FAILED) {
          const failure = new BackendError(
            "operation_failed",
            task.error || "factor evaluation failed",
          );
          if (openPage.current === targetFactor) fail(failure);
          else notify(failure, { namespace: "asterion.terminal.research" });
        }
      } else if (isComposite) {
        trackedCompositeRef.current = "";
        if (task.state === RunState.SUCCEEDED) {
          void readKept();
          if (!openPage.current) setReport(task.id);
        } else if (task.state === RunState.FAILED) {
          const failure = new BackendError(
            "operation_failed",
            task.error || "factor evaluation failed",
          );
          if (!openPage.current) fail(failure);
          else notify(failure, { namespace: "asterion.terminal.research" });
        }
      } else if (isBatch) {
        trackedBatchRef.current = "";
        delete lastBatchDoneRef.current[task.id];
        void readKept();
        if (task.state === RunState.FAILED && task.error) {
          const failure = new BackendError("operation_failed", task.error);
          if (!openPage.current) fail(failure);
          else notify(failure, { namespace: "asterion.terminal.research" });
        }
      } else if (task.state === RunState.SUCCEEDED || task.kind === TaskKind.FACTOR_BATCH) {
        void readKept();
      }
    }
  }, [labActivity?.active, labActivity?.recent, readKept, fail, setReport, setTab]);

  const cancelFactorTask = async (id: string) => {
    setCancellingId(id);
    await attempt(async () => {
      await lab({ case: "cancelFactor", value: { id } });
      await labActivity?.refresh();
    });
    setCancellingId(previous => (previous === id ? "" : previous));
  };

  const evaluate = async () => {
    if (!factor) return;
    setSubmitting(true);
    setSaved([]);
    await attempt(async () => {
      const written = await source.save();
      setSaved(written);
      if (written.length) await read();
      const id = identity(factor.id);
      const request = factorRequest(form, repository, factor);
      trackedSingleRef.current = { id, factorId: factor.id };
      await lab({
        case: "startFactor",
        value: { ...request, id },
      });
      await labActivity?.refresh();
    });
    setSubmitting(false);
  };
  const activeSelected = selected.filter(id => modules?.factors.some(item => item.id === id));
  const evaluateBatch = async () => {
    const targets = activeSelected
      .map(id => modules?.factors.find(item => item.id === id))
      .filter((entry): entry is NonNullable<typeof entry> => entry !== undefined);
    if (!targets.length) return;
    setSubmitting(true);
    setSaved([]);
    await attempt(async () => {
      const requests = targets.map((entry, index) => ({
        ...factorRequest(form, repository, entry, { useDefaults: true, extraFactors: [] }),
        id: identity(entry.id, String(index + 1)),
      }));
      const written = await source.save();
      setSaved(written);
      if (written.length) await read();
      const batchId = identity("factors");
      trackedBatchRef.current = batchId;
      await lab({
        case: "startFactorBatch",
        value: { id: batchId, evaluations: requests, processes: 0 },
      });
      await labActivity?.refresh();
    });
    setSubmitting(false);
  };
  const evaluateComposite = async () => {
    const targets = activeSelected
      .map(id => modules?.factors.find(item => item.id === id))
      .filter((entry): entry is NonNullable<typeof entry> => entry !== undefined);
    if (targets.length < 2) return;
    setSubmitting(true);
    setSaved([]);
    await attempt(async () => {
      const primary = targets[0]!;
      const request = factorRequest(form, repository, primary, {
        useDefaults: true,
        extraFactors: targets.slice(1).map(entry => entry.id),
      });
      const written = await source.save();
      setSaved(written);
      if (written.length) await read();
      const id = identity(primary.id, "mix");
      trackedCompositeRef.current = id;
      await lab({ case: "startFactor", value: { ...request, id } });
      await labActivity?.refresh();
    });
    setSubmitting(false);
  };
  const remove = (id: string) =>
    void attempt(async () => {
      await lab({ case: "deleteFactorResult", value: { id } });
      deleteAnnotations([id]);
      setReport("");
      await readKept();
    });
  const removeMany = async (ids: string[]) => {
    await attempt(async () => {
      await Promise.all(ids.map(id => lab({ case: "deleteFactorResult", value: { id } })));
      deleteAnnotations(ids);
      if (ids.includes(report)) setReport("");
      setComparing(previous => previous.filter(id => !ids.includes(id)));
      await readKept();
    });
  };
  useEffect(() => {
    if (!chosen || !modules || modules.factors.some(item => item.id === chosen)) return;
    const [stem] = chosen.split(".");
    const renamed = modules.factors.find(item => item.id.startsWith(`${stem}.`));
    if (renamed) setChosen(renamed.id);
  }, [chosen, modules, setChosen]);

  // The open factor's evaluation follows the settings in use.
  useEffect(() => {
    if (shownScope.current === scopeText) return;
    shownScope.current = scopeText;
    if (library || !chosen) return;
    setReport(own(chosen).find(item => inScope?.(item))?.id ?? "");
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scopeText]);

  // Deep-link from a generated strategy's lineage record or completion notification back to its factor evaluation.
  const openFactorResultId = context.workspaceParams?.factorResult;
  useEffect(() => {
    if (!openFactorResultId) return;
    let active = true;
    const openFound = (itemsList: readonly FactorResult[]) => {
      const found = itemsList.find(item => item.id === openFactorResultId);
      if (found && !isCompositeResult(found) && found.evaluation?.factor) {
        const target = found.evaluation.factor;
        setChosen(target);
        source.open(`factors/${target.split(".")[0]}.py`);
        setView("detail");
        setReport(found.id);
        setTab("result");
      } else {
        setView("library");
        setReport(openFactorResultId);
      }
      context.navigate("workspace.lab-factors", {});
    };
    if (kept.some(item => item.id === openFactorResultId)) {
      openFound(kept);
      return;
    }
    void readKept().then(itemsList => {
      if (active) openFound(itemsList);
    });
    return () => {
      active = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [openFactorResultId]);

  const [folded, setFolded] = useConfigFold();
  const menu = useModuleMenu({
    repository,
    source,
    deleted: path => {
      forget(path);
      const stem = path.replace(/^factors\//, "").replace(/\.py$/, "");
      if (chosen.startsWith(`${stem}.`)) setChosen("");
      openLibrary();
      setSelected(previous => previous.filter(id => !id.startsWith(`${stem}.`)));
      void read();
    },
  });
  const combinationField = (className?: string) => (
    <label className={className}>
      <span>{t("合成方式")}</span>
      <select
        value={form.combination ?? "equal"}
        onChange={event => change({ combination: event.target.value as FactorForm["combination"] })}
      >
        <option value="equal">{t("等权合成")}</option>
        <option value="ic_weighted">{t("IC 加权合成")}</option>
      </select>
    </label>
  );
  const missingData = parseDataGap(error, { minutes: isMinutes });
  const errorAction = missingData
    ? {
        label: t("去采集"),
        onClick: () =>
          context.navigate("workspace.data-dataset", { params: dataGapParams(missingData) }),
      }
    : undefined;
  const librarySwitcher = (
    <div className="lab-segmented" role="group" aria-label={t("因子视图")}>
      <button
        type="button"
        aria-pressed={libraryTab === "library"}
        onClick={() => {
          setComparing([]);
          setLibraryTab("library");
        }}
      >
        {t("因子库")}
      </button>
      <button
        type="button"
        aria-pressed={libraryTab === "records"}
        onClick={() => {
          setComparing([]);
          setLibraryTab("records");
        }}
      >
        {kept.length > 0 ? t("评价记录（{count}）", { count: kept.length }) : t("评价记录")}
      </button>
    </div>
  );
  const openDataset = (params: Record<string, string>) =>
    context.navigate("workspace.data-dataset", { params });
  const savedNote = saved.length > 0 && (
    <p className="subtle">{t("已保存 {files}", { files: saved.join(listSeparator()) })}</p>
  );
  const sameName = (item: ModuleItem) => items.filter(other => other.name === item.name).length > 1;
  const reportName = shownResult ? resultName(shownResult) : recordName(report);
  const showsResult = !library && tab !== "code" && Boolean(report);
  const onEvaluateShortcut = (event: KeyboardEvent) => {
    if (
      !library &&
      factor &&
      !submitting &&
      !activeSingleTask &&
      !context.busy &&
      (event.metaKey || event.ctrlKey) &&
      event.key === "Enter"
    ) {
      event.preventDefault();
      void evaluate();
    }
  };
  return (
    <section
      className="lab-research"
      data-folded={(!library && folded) || undefined}
      onKeyDown={onEvaluateShortcut}
    >
      <FactorScopeBar
        context={context}
        form={form}
        begin={scopeBegin}
        end={scopeEnd}
        conditions={conditions}
        catalog={catalog}
      />
      {!library && (
        <header className="lab-report-header lab-page-head">
          <button type="button" className="lab-link" onClick={openLibrary}>
            {t("← 因子库")}
          </button>
          {factor ? (
            <select
              className="lab-strategy-switch"
              aria-label={t("切换因子")}
              value={chosen}
              onChange={event => choose(event.target.value)}
            >
              {items.map(item => (
                <option key={item.id} value={item.id}>
                  {sameName(item) ? `${item.name} · ${item.module}` : item.name}
                </option>
              ))}
            </select>
          ) : (
            <h2>{source.path}</h2>
          )}
          {factor && (
            <div className="lab-segmented" role="group" aria-label={t("因子")}>
              {(
                [
                  ["result", "评价"],
                  ["code", "代码"],
                  ["split", "分屏"],
                ] as const
              ).map(([key, label]) => (
                <button
                  key={key}
                  type="button"
                  className={key === "split" ? "lab-split-toggle" : undefined}
                  aria-pressed={tab === key}
                  onClick={() => setTab(key)}
                >
                  {t(label)}
                </button>
              ))}
            </div>
          )}
          {showsResult && <h2>{shownResult ? recordTime(shownResult.id) : reportName}</h2>}
          {showsResult && shownResult && inScope && !inScope(shownResult) && (
            <StatusBadge tone="neutral">{t("条件不同")}</StatusBadge>
          )}
          <span className="lab-spacer" />
          {showsResult && shownResult && (
            <button type="button" onClick={() => setGenerating(shownResult)}>
              {t("生成策略")}
            </button>
          )}
          {showsResult && <DeleteRecord name={report} remove={() => remove(report)} />}
        </header>
      )}
      {menu.overlay}
      {menu.copying && (
        <NewModule
          key={menu.copying.id}
          hideTrigger
          folder="factors"
          repository={repository}
          copying={menu.copying}
          cancelCopy={menu.clearCopy}
          created={(path, id) => {
            openCode(path, id);
            void read();
          }}
        />
      )}
      {generating && (
        <NewModule
          key={`gen:${generating.id}`}
          hideTrigger
          folder="strategies"
          repository={repository}
          generated={{
            title: "从因子生成策略",
            name: factorStrategyStem(generating),
            build: name => factorStrategyTemplate(generating, name),
          }}
          cancelGenerated={() => setGenerating(null)}
          created={(_path, id) => {
            const target = generating;
            setGenerating(null);
            const symbols = (
              target.symbols.length ? target.symbols : (target.evaluation?.symbols ?? [])
            ).join(", ");
            const params: Record<string, string> = {};
            if (id) params.openStrategy = id;
            if (symbols) params.symbols = symbols;
            if (target.evaluation?.firstDay) params.begin = target.evaluation.firstDay;
            if (target.evaluation?.lastDay) params.end = target.evaluation.lastDay;
            if (target.evaluation?.holdoutDay) params.holdoutDay = target.evaluation.holdoutDay;
            context.navigate("workspace.lab-strategies", { params });
          }}
        />
      )}
      {library && comparing.length >= 2 ? (
        <FactorComparison ids={comparing} onBack={() => setComparing([])} onOpen={openRecord} />
      ) : library && report ? (
        <div className="lab-center lab-runs-view">
          <header className="lab-report-header">
            <button type="button" className="lab-link" onClick={() => setReport("")}>
              {libraryTab === "records" ? t("← 评价记录") : t("← 因子库")}
            </button>
            <h2>{reportName}</h2>
            <span className="lab-spacer" />
            {shownResult && (
              <button type="button" onClick={() => setGenerating(shownResult)}>
                {t("生成策略")}
              </button>
            )}
            <DeleteRecord name={report} remove={() => remove(report)} />
          </header>
          <Report key={report} id={report} openDataset={openDataset} />
        </div>
      ) : library && libraryTab === "records" ? (
        <FactorRecords
          records={kept}
          annotations={annotations}
          search={recordsSearch}
          setSearch={setRecordsSearch}
          onScope={inScope}
          busy={submitting || Boolean(context.busy)}
          onOpen={openRecord}
          onCompare={setComparing}
          onDeleteMany={removeMany}
          switcher={librarySwitcher}
          notices={
            <ActionError
              error={error}
              namespace="asterion.terminal.research"
              onDismiss={clear}
              action={errorAction}
            />
          }
        />
      ) : library ? (
        <FactorLibrary
          items={items}
          switcher={librarySwitcher}
          search={search}
          setSearch={setSearch}
          chosen={chosen}
          selected={activeSelected}
          setSelected={setSelected}
          latest={latest}
          onScope={inScope}
          composites={composites}
          busy={submitting || Boolean(context.busy)}
          batchProgress={
            activeBatchTask ? { done: activeBatchTask.done, total: activeBatchTask.total } : null
          }
          compositeRunning={Boolean(activeCompositeTask)}
          cancelling={
            Boolean(activeBatchTask && cancellingId === activeBatchTask.id) ||
            Boolean(activeCompositeTask && cancellingId === activeCompositeTask.id)
          }
          onBatchEvaluate={() => void evaluateBatch()}
          onCancelBatch={() => {
            if (activeBatchTask) void cancelFactorTask(activeBatchTask.id);
          }}
          onCompositeEvaluate={() => void evaluateComposite()}
          onCancelComposite={() => {
            if (activeCompositeTask) void cancelFactorTask(activeCompositeTask.id);
          }}
          open={choose}
          openComposite={setReport}
          contextTarget={menu.target}
          onMenu={menu.open}
          combination={combinationField("lab-inline")}
          newModule={
            <NewModule
              folder="factors"
              repository={repository}
              created={(path, id) => {
                openCode(path, id);
                void read();
              }}
            />
          }
          notices={
            <>
              <ActionError
                error={error}
                namespace="asterion.terminal.research"
                onDismiss={clear}
                action={errorAction}
              />
              {savedNote}
              {modules && !modules.factors.length && <p className="subtle">{t("还没有因子")}</p>}
              <ModuleErrors
                folder="factors"
                modules={modules}
                contextTarget={menu.target}
                open={openCode}
                onMenu={menu.open}
              />
            </>
          }
        />
      ) : (
        <div className={`lab-detail${tab === "split" ? " lab-detail-split" : ""}`}>
          {(tab === "code" || tab === "split") && (
            <LabCentre
              source={source}
              repository={repository}
              modules={modules}
              focus={chosen}
              changed={() => void read()}
              onDuplicate={source.path.startsWith("factors/") ? menu.copy : undefined}
              onDelete={path => menu.remove(path)}
            />
          )}
          {(tab === "result" || tab === "split") &&
            (report ? (
              <div className="lab-center lab-runs-view">
                <Report
                  key={report}
                  id={report}
                  repository={repository}
                  drafts={source.drafts}
                  openDataset={openDataset}
                />
              </div>
            ) : (
              <div className="lab-placeholder">
                <p>{t("还没有评价")}</p>
              </div>
            ))}
        </div>
      )}
      {factor && !library && (
        <aside className="lab-config" aria-label={t("评价")}>
          <h2>
            {!folded && t("参数")}
            <ConfigFold folded={folded} fold={setFolded} />
          </h2>
          {!folded && (
            <>
              {factor.parameters.length > 0 && (
                <div className="lab-form lab-parameters">
                  {factor.parameters.map(parameter => (
                    <label
                      key={parameter.name}
                      className={factor.parameters.length === 1 ? "wide" : undefined}
                    >
                      <span>{parameter.name}</span>
                      <input
                        className="lab-param"
                        placeholder={defaultText(parameter)}
                        spellCheck={false}
                        autoCorrect="off"
                        autoCapitalize="none"
                        value={form.params[parameter.name] ?? ""}
                        onChange={event =>
                          change({
                            params: { ...form.params, [parameter.name]: event.target.value },
                          })
                        }
                      />
                    </label>
                  ))}
                </div>
              )}
              {items.length > 1 && (
                <>
                  <FactorPickerSection
                    title={count => t("合成因子（{count}）", { count })}
                    searchLabel={t("搜索合成因子")}
                    items={items.filter(item => item.id !== factor.id)}
                    value={form.extraFactors ?? []}
                    onChange={extraFactors => change({ extraFactors })}
                  >
                    {combinationField("wide")}
                  </FactorPickerSection>
                  <FactorPickerSection
                    title={count => t("正交剥离基准（{count}）", { count })}
                    searchLabel={t("搜索正交基准因子")}
                    items={items.filter(
                      item => item.id !== factor.id && !(form.extraFactors ?? []).includes(item.id),
                    )}
                    value={form.orthogonalizeAgainst ?? []}
                    onChange={orthogonalizeAgainst => change({ orthogonalizeAgainst })}
                  />
                </>
              )}
              {evaluations.length > 0 &&
                (() => {
                  const hasOtherScope = Boolean(
                    inScope && evaluations.some(item => !inScope(item)),
                  );
                  const shownEvaluations =
                    onlyScopeRecords && inScope
                      ? evaluations.filter(item => inScope(item))
                      : evaluations;
                  return (
                    <Section title={t("评价记录（{count}）", { count: evaluations.length })} open>
                      {hasOtherScope && (
                        <div className="lab-evaluations-bar">
                          <label className="lab-inline lab-check-inline">
                            <input
                              type="checkbox"
                              checked={onlyScopeRecords}
                              onChange={event => setOnlyScopeRecords(event.target.checked)}
                            />
                            <span>{t("仅当前条件")}</span>
                          </label>
                        </div>
                      )}
                      <ul className="lab-evaluations" aria-label={t("评价记录")}>
                        {shownEvaluations.map(item => {
                          const ic = item.horizons[0]?.mean;
                          const label = isCompositeResult(item)
                            ? compositeName(item)
                            : Object.entries(item.evaluation?.params ?? {})
                                .map(([name, value]) => `${name}=${String(value.value.value)}`)
                                .join(" ") || recordName(item.id);
                          const ann = annotations[item.id];
                          const offScope = Boolean(inScope && !inScope(item));
                          const pTag = factorPeriodTag(item.evaluation?.period);
                          const count = item.symbols.length || item.evaluation?.symbols.length || 0;
                          const meta = [
                            pTag,
                            count ? t("{count} 个品种", { count }) : "",
                            item.evaluation?.firstDay && item.evaluation?.lastDay
                              ? `${item.evaluation.firstDay} – ${item.evaluation.lastDay}`
                              : "",
                          ]
                            .filter(Boolean)
                            .join(" · ");
                          return (
                            <li key={item.id}>
                              <button
                                type="button"
                                aria-pressed={item.id === report}
                                data-off-scope={offScope || undefined}
                                title={ann?.note || item.id}
                                onClick={() => {
                                  setReport(item.id);
                                  setTab(previous => (previous === "split" ? "split" : "result"));
                                }}
                              >
                                <span className="lab-eval-main">
                                  <span>
                                    {ann?.starred && <span className="lab-star-mark">★ </span>}
                                    {label}
                                  </span>
                                  <small>{meta}</small>
                                </span>
                                <span className={`numeric ${signed(ic)}`}>{metric(ic, false)}</span>
                              </button>
                            </li>
                          );
                        })}
                      </ul>
                    </Section>
                  );
                })()}
              {users.length > 0 && (
                <Section title={t("导入本模块的策略（{count}）", { count: users.length })}>
                  <ul className="lab-evaluations" aria-label={t("导入本模块的策略")}>
                    {users.map(item => (
                      <li key={item.id}>
                        <button
                          type="button"
                          onClick={() => {
                            setReport("");
                            setTab("code");
                            source.jump(`strategies/${item.id.split(".")[0]}.py`);
                          }}
                        >
                          {item.id.split(".")[1]}
                        </button>
                      </li>
                    ))}
                  </ul>
                </Section>
              )}
              <footer className="lab-submit">
                <div className="lab-submit-row">
                  <button
                    type="button"
                    className="primary"
                    disabled={submitting || Boolean(activeSingleTask) || context.busy}
                    onClick={() => void evaluate()}
                  >
                    {submitting || activeSingleTask ? t("正在评价…") : t("评价")}
                  </button>
                  {activeSingleTask && (
                    <button
                      type="button"
                      className="lab-link"
                      disabled={cancellingId === activeSingleTask.id}
                      onClick={() => void cancelFactorTask(activeSingleTask.id)}
                    >
                      {t("取消")}
                    </button>
                  )}
                </div>
                {savedNote}
                <ActionError
                  error={error}
                  namespace="asterion.terminal.research"
                  onDismiss={clear}
                  action={errorAction}
                />
              </footer>
            </>
          )}
        </aside>
      )}
    </section>
  );
}

// Factors picked by search for combination or orthogonalization.
function FactorPickerSection({
  title,
  searchLabel,
  items,
  value,
  onChange,
  children,
}: {
  title: (count: number) => string;
  searchLabel: string;
  items: readonly ModuleItem[];
  value: readonly string[];
  onChange: (next: string[]) => void;
  children?: ReactNode;
}) {
  const [search, setSearch] = useState("");
  const count = items.filter(item => value.includes(item.id)).length;
  return (
    <Section title={title(count)}>
      {children}
      <input
        type="search"
        className="wide"
        aria-label={searchLabel}
        placeholder={t("搜索因子")}
        spellCheck={false}
        value={search}
        onChange={event => setSearch(event.target.value)}
      />
      <div className="lab-factor-picker wide">
        {filterModuleItems(items, search).map(item => (
          <label key={item.id} className="checkbox" title={item.description}>
            <input
              type="checkbox"
              checked={value.includes(item.id)}
              onChange={event =>
                onChange(
                  event.target.checked ? [...value, item.id] : value.filter(id => id !== item.id),
                )
              }
            />
            <span>{item.name}</span>
            <small>{item.module}</small>
          </label>
        ))}
      </div>
    </Section>
  );
}

type LibraryKey = "ic" | "holdoutIc" | "ir" | "t" | "turnover" | "evaluated";
const sections = ["", "条件不同", "未评价"] as const;

const figuresOf = (result?: FactorResult) => {
  const first = result?.horizons[0];
  return {
    ic: first?.mean,
    holdoutIc: result?.holdoutHorizons[0]?.mean,
    ir: first?.ir,
    t: first?.t,
    turnover: factorTurnover(result),
    evaluated: result ? Number(result.evaluatedMs) : undefined,
  };
};

function Figures({ result }: { result?: FactorResult }) {
  const value = figuresOf(result);
  return (
    <>
      <td className={`numeric ${signed(value.ic)}`}>{result ? metric(value.ic, false) : "—"}</td>
      <td className={`numeric ${signed(value.holdoutIc)}`}>
        {result ? metric(value.holdoutIc, false) : "—"}
      </td>
      <td className="numeric">{result ? metric(value.ir, false) : "—"}</td>
      <td className="numeric">{result ? metric(value.t, false) : "—"}</td>
      <td className="numeric">{result ? metric(value.turnover, true) : "—"}</td>
      <td className="subtle numeric">{result ? shortTime(result.evaluatedMs) : "—"}</td>
    </>
  );
}

// Every factor beside its evaluation on the current settings. Results on other
// settings and factors without one are listed apart, so they never rank.
function FactorLibrary({
  items: all,
  switcher,
  search,
  setSearch,
  chosen,
  selected,
  setSelected,
  latest,
  onScope,
  composites,
  busy,
  batchProgress,
  compositeRunning,
  cancelling,
  onBatchEvaluate,
  onCancelBatch,
  onCompositeEvaluate,
  onCancelComposite,
  open,
  openComposite,
  contextTarget,
  onMenu,
  combination,
  newModule,
  notices,
}: {
  items: readonly ModuleItem[];
  switcher?: ReactNode;
  search: string;
  setSearch: (search: string) => void;
  chosen: string;
  selected: readonly string[];
  setSelected: (next: string[] | ((previous: string[]) => string[])) => void;
  latest: (id: string) => FactorResult | undefined;
  // Absent until the settings name a symbol.
  onScope?: (result: FactorResult) => boolean;
  composites: readonly FactorResult[];
  busy: boolean;
  batchProgress: { done: number; total: number } | null;
  compositeRunning: boolean;
  cancelling: boolean;
  onBatchEvaluate: () => void;
  onCancelBatch: () => void;
  onCompositeEvaluate: () => void;
  onCancelComposite: () => void;
  open: (id: string) => void;
  openComposite: (id: string) => void;
  contextTarget?: string;
  onMenu: (path: string, x: number, y: number, symbol?: string, target?: string) => void;
  combination: ReactNode;
  newModule: ReactNode;
  notices: ReactNode;
}) {
  const { heading, sorted, status } = useLibrarySort<LibraryKey>("ic");
  const [moduleFilter, setModuleFilter] = useState("");
  const moduleNames = [...new Set(all.map(item => item.module))].sort();
  // Deleting a module's last factor removes its option.
  const activeModule = moduleNames.includes(moduleFilter) ? moduleFilter : "";
  const searched = filterModuleItems(all, search);
  const items = activeModule ? searched.filter(item => item.module === activeModule) : searched;
  const query = search.trim().toLowerCase();
  const shownComposites = activeModule
    ? []
    : composites.filter(item => compositeName(item).toLowerCase().includes(query));
  const selectAllRef = useRef<HTMLInputElement>(null);
  const shownIds = items.map(item => item.id);
  const selectedShown = shownIds.filter(id => selected.includes(id));
  const allShown = shownIds.length > 0 && selectedShown.length === shownIds.length;
  const someShown = selectedShown.length > 0 && !allShown;
  useEffect(() => {
    if (selectAllRef.current) selectAllRef.current.indeterminate = someShown;
  }, [someShown]);

  const rows = sorted(
    items.map(item => {
      const result = latest(item.id);
      return { item, result, place: !result ? 2 : !onScope || onScope(result) ? 0 : 1 };
    }),
    (row, key) => figuresOf(row.result)[key],
  );
  const toggleOne = (id: string, checked: boolean) =>
    setSelected(previous =>
      checked
        ? previous.includes(id)
          ? previous
          : [...previous, id]
        : previous.filter(x => x !== id),
    );
  const toggleAll = (checked: boolean) =>
    setSelected(previous =>
      checked
        ? [...new Set([...previous, ...shownIds])]
        : previous.filter(id => !shownIds.includes(id)),
    );
  const row = ({ item, result }: (typeof rows)[number]) => {
    const checked = selected.includes(item.id);
    return (
      <tr
        key={item.id}
        className={checked ? "selected" : item.id === chosen ? "current" : undefined}
        data-empty={result ? undefined : true}
        data-context={contextTarget === item.id || undefined}
        title={[item.description, item.inputs?.join(listSeparator())].filter(Boolean).join("\n")}
        onClick={() => open(item.id)}
        onContextMenu={event => {
          event.preventDefault();
          onMenu(`factors/${item.module}.py`, event.clientX, event.clientY, item.name, item.id);
        }}
      >
        <td className="lab-check-col" onClick={event => event.stopPropagation()}>
          <input
            type="checkbox"
            aria-label={t("选择 {name}", { name: item.id })}
            checked={checked}
            onChange={event => toggleOne(item.id, event.target.checked)}
          />
        </td>
        <th scope="row">
          <button
            type="button"
            className="lab-link strong"
            onClick={event => {
              event.stopPropagation();
              open(item.id);
            }}
          >
            {item.name}
          </button>
        </th>
        <td className="subtle">{item.module}</td>
        <Figures result={result} />
      </tr>
    );
  };
  return (
    <div className="lab-center lab-library-view">
      <div className="lab-toolbar">
        {switcher}
        <input
          type="search"
          aria-label={t("搜索因子")}
          placeholder={t("搜索因子")}
          spellCheck={false}
          autoCorrect="off"
          autoCapitalize="none"
          value={search}
          onChange={event => setSearch(event.target.value)}
        />
        <label className="lab-inline">
          <span>{t("模块")}</span>
          <select value={activeModule} onChange={event => setModuleFilter(event.target.value)}>
            <option value="">{t("全部")}</option>
            {moduleNames.map(name => (
              <option key={name} value={name}>
                {name}
              </option>
            ))}
          </select>
        </label>
        <span className="lab-spacer" />
        {selected.length > 0 && (
          <button
            type="button"
            className="lab-link"
            disabled={busy}
            onClick={() => setSelected([])}
          >
            {t("清空选择")}
          </button>
        )}
        {selected.length >= 2 && combination}
        {newModule}
        <button
          type="button"
          disabled={busy || compositeRunning || Boolean(batchProgress) || selected.length < 2}
          onClick={onCompositeEvaluate}
        >
          {compositeRunning
            ? t("正在评价…")
            : selected.length
              ? t("相关性与合成（{count}）", { count: selected.length })
              : t("相关性与合成")}
        </button>
        {compositeRunning && (
          <button
            type="button"
            className="lab-link"
            disabled={cancelling}
            onClick={onCancelComposite}
          >
            {t("取消")}
          </button>
        )}
        <button
          type="button"
          className="primary"
          disabled={busy || Boolean(batchProgress) || compositeRunning || selected.length === 0}
          onClick={onBatchEvaluate}
        >
          {batchProgress
            ? t("正在评价 {done} / {total}…", batchProgress)
            : selected.length
              ? t("批量评价（{count}）", { count: selected.length })
              : t("批量评价")}
        </button>
        {batchProgress && (
          <button type="button" className="lab-link" disabled={cancelling} onClick={onCancelBatch}>
            {t("取消")}
          </button>
        )}
      </div>
      {notices}
      {status}
      {rows.length > 0 || shownComposites.length > 0 ? (
        <div
          className="lab-scroll lab-library-scroll"
          tabIndex={0}
          role="region"
          aria-label={t("因子库")}
        >
          <table className="lab-table lab-records lab-library-table" aria-label={t("因子库")}>
            <thead>
              <tr>
                <th scope="col" className="lab-check-col">
                  <input
                    ref={selectAllRef}
                    type="checkbox"
                    aria-label={t("全选因子")}
                    checked={allShown}
                    disabled={shownIds.length === 0}
                    onChange={event => toggleAll(event.target.checked)}
                  />
                </th>
                <th scope="col" className="lab-name-col">
                  {t("因子")}
                </th>
                <th scope="col">{t("模块")}</th>
                {heading("ic", t("IC 均值"), t("最近一次评价的第一个持有期"))}
                {heading("holdoutIc", t("留出期 IC"), t("最近一次评价的留出期第一个持有期"))}
                {heading("ir", "ICIR")}
                {heading("t", t("t 值"))}
                {heading("turnover", t("换手率"), t("各分组日均换手率均值"))}
                {heading("evaluated", t("评价时间"))}
              </tr>
            </thead>
            {sections.map((title, place) => {
              const group = rows.filter(entry => entry.place === place);
              if (!group.length) return null;
              return (
                <tbody key={place}>
                  {title && (
                    <tr className="lab-library-group">
                      <th scope="colgroup" colSpan={9}>
                        {t(title)}
                      </th>
                    </tr>
                  )}
                  {group.map(row)}
                </tbody>
              );
            })}
            {shownComposites.length > 0 && (
              <tbody>
                <tr className="lab-library-group">
                  <th scope="colgroup" colSpan={9}>
                    {t("多因子合成记录")}
                  </th>
                </tr>
                {shownComposites.map(item => (
                  <tr
                    key={item.id}
                    title={item.factorNames.join(" + ") || item.id}
                    onClick={() => openComposite(item.id)}
                  >
                    <td className="lab-check-col" />
                    <th scope="row">
                      <button
                        type="button"
                        className="lab-link strong"
                        onClick={event => {
                          event.stopPropagation();
                          openComposite(item.id);
                        }}
                      >
                        {compositeName(item)}
                      </button>
                    </th>
                    <td className="subtle">
                      {t("{count} 个因子", {
                        count:
                          item.factorNames.length ||
                          1 + (item.evaluation?.extraFactors.length ?? 0),
                      })}{" "}
                      ·{" "}
                      {item.evaluation?.combination === "ic_weighted"
                        ? t("IC 加权合成")
                        : t("等权合成")}
                    </td>
                    <Figures result={item} />
                  </tr>
                ))}
              </tbody>
            )}
          </table>
        </div>
      ) : (
        all.length > 0 && <p className="subtle">{t("没有匹配的模块")}</p>
      )}
    </div>
  );
}

function Report({
  id,
  repository,
  drafts,
  openDataset,
}: {
  id: string;
  repository?: string;
  drafts?: Record<string, string>;
  openDataset?: (params: Record<string, string>) => void;
}) {
  const [result, setResult] = useState<FactorResult | null>(null);
  const [loadedCode, setLoadedCode] = useState<string | undefined>(undefined);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    setLoadedCode(undefined);
    void lab({ case: "getFactorResult", value: { id } })
      .then(reply => {
        if (!active) return;
        setResult(reply);
        if (repository && !isCompositeResult(reply) && reply.evaluation?.factor) {
          const path = `factors/${reply.evaluation.factor.split(".")[0]}.py`;
          void lab({ case: "readSource", value: { repository, path } })
            .then(sourceReply => {
              if (active) setLoadedCode(sourceReply.content);
            })
            .catch(() => {});
        }
      })
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [id, repository]);
  useErrorNotification(error, "asterion.terminal.research");
  if (error) return null;
  if (!result) return <p className="subtle">{t("正在读取结果…")}</p>;
  const modulePath =
    repository && !isCompositeResult(result) && result.evaluation?.factor
      ? `factors/${result.evaluation.factor.split(".")[0]}.py`
      : "";
  const currentCode = modulePath ? (drafts?.[modulePath] ?? loadedCode) : undefined;
  return <FactorResultView result={result} currentCode={currentCode} openDataset={openDataset} />;
}
