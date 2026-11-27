import { StatusBadge } from "../../src/ui/StatusBadge";
import { useCallback, useEffect, useId, useRef, useState, type KeyboardEvent } from "react";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { lab } from "../../src/bridge/lab";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { RunSummary } from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { useLabActiveReport } from "../../src/host/lab";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import { dataGapParams, parseDataGap } from "./data-gap";
import { SLIPPAGE_STRESS_TICKS } from "./research-analytics";
import {
  defaultProcesses,
  defaultText,
  formFromRun,
  identity,
  onScope,
  recordName,
  runName,
  runRequest,
  runState,
  savedForm,
  scopeOf,
  slippageKey,
  slippageStressBase,
  type RunForm,
} from "./research-lab";
import { symbolsOf } from "./research-storage";
import {
  ConfigFold,
  LabCentre,
  ModuleErrors,
  type ModuleItem,
  NewModule,
  Section,
  useAnnotations,
  useConfigFold,
  useModuleMenu,
  useModules,
  useSource,
} from "./LabParts";
import { metric, runTones, signed, states } from "./result-parts";
import { RunResult } from "./RunResult";
import { RunScope, useRunConditions } from "./RunScope";
import { strategyResult, StrategyLibrary, useStrategyBatch } from "./StrategyLibrary";
import { SweepResult } from "./SweepResult";
import { datePlaceholders, useStoredCatalog } from "./SymbolPicker";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

type ActiveReport = { kind: "run" | "sweep"; id: string; fromSweep?: string } | null;

// The strategy view: every strategy ranked on one set of backtest settings, and
// one strategy's result and code with its parameters beside them.
export function StrategyLab(context: TerminalContext) {
  const modeGroup = useId();
  const repository = context.snapshot?.research_directory ?? "";
  const {
    modules: strategies,
    read: readStrategies,
    forget: forgetStrategy,
  } = useModules(repository);
  const [chosen, setChosen] = useWorkspaceDraft(`strategy:${repository}`, "");
  const [report, setReport] = useWorkspaceDraft<ActiveReport>(
    `strategy-report:${repository}`,
    null,
  );
  const [form, setForm] = useWorkspaceDraft<RunForm>("run-form", savedForm);
  const conditions = useRunConditions(setForm);
  const scopeText = JSON.stringify(scopeOf(form));
  const shownScope = useRef(scopeText);
  const [view, setView] = useWorkspaceDraft<"library" | "detail">("strategy-view", "library");
  const [tab, setTab] = useWorkspaceDraft<"result" | "code" | "split">("strategy-tab", "code");
  const [search, setSearch] = useWorkspaceDraft("modules-search:strategies", "");
  const source = useSource(repository);
  const [runs, setRuns] = useState<RunSummary[]>([]);
  const readRecentRuns = useCallback(async () => {
    const reply = await lab({ case: "listRecentRuns", value: {} });
    setRuns(reply.runs);
  }, []);
  useEffect(() => {
    void readRecentRuns().catch(failure =>
      notify(failure, { namespace: "asterion.terminal.research" }),
    );
  }, [readRecentRuns]);

  const loadRunId = context.workspaceParams?.loadRun;
  const openStrategyId = context.workspaceParams?.openStrategy;
  useEffect(() => {
    if (!chosen || !strategies || strategies.strategies.some(item => item.id === chosen)) return;
    const [stem] = chosen.split(".");
    const renamed = strategies.strategies.find(item => item.id.startsWith(`${stem}.`));
    if (renamed) setChosen(renamed.id);
  }, [chosen, strategies, setChosen]);

  useEffect(() => {
    if (!openStrategyId) return;
    const [stem] = openStrategyId.split(".");
    if (!stem) return;
    setChosen(openStrategyId);
    source.open(`strategies/${stem}.py`);
    setView("detail");
    setReport(null);
    setTab("code");
    const symbols = context.workspaceParams?.symbols ?? "";
    const begin = context.workspaceParams?.begin ?? "";
    const end = context.workspaceParams?.end ?? "";
    const holdoutDay = context.workspaceParams?.holdoutDay ?? "";
    if (symbols || begin || end || holdoutDay) {
      const symbolList = symbolsOf(symbols);
      const next: RunForm = {
        ...form,
        ...(symbols ? { symbols } : {}),
        ...(begin ? { begin } : {}),
        ...(end ? { end } : {}),
        holdoutDay,
        validation: holdoutDay ? "holdout" : "none",
        multiSymbolMode: symbolList.length > 1 ? "portfolio" : undefined,
        params: {},
      };
      setForm(next);
      shownScope.current = JSON.stringify(conditions.adopt(scopeOf(next)));
    } else {
      setForm(previous => ({ ...previous, params: {} }));
    }
    void readStrategies();
    context.navigate("workspace.lab-strategies", {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [openStrategyId]);

  useEffect(() => {
    if (!loadRunId) return;
    let active = true;
    void lab({ case: "getRun", value: { id: loadRunId } })
      .then(detail => {
        if (!active) return;
        const loaded = formFromRun(detail);
        if (loaded.strategy) {
          setChosen(loaded.strategy);
          source.open(`strategies/${loaded.strategy.split(".")[0]}.py`);
        }
        setView("detail");
        setReport({ kind: "run", id: loadRunId });
        setTab("result");
        setForm(loaded.form);
        // This run is the one to show, whatever else its settings have produced.
        shownScope.current = JSON.stringify(conditions.adopt(scopeOf(loaded.form)));
        context.navigate("workspace.lab-strategies", {});
      })
      .catch(failure => active && notify(failure, { namespace: "asterion.terminal.research" }));
    return () => {
      active = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [loadRunId]);

  const refreshRecentRuns = useCallback(() => {
    void readRecentRuns().catch(() => {});
  }, [readRecentRuns]);
  const {
    activeRun,
    activeSweep,
    refresh: refreshReport,
  } = useLabActiveReport(report, refreshRecentRuns);

  const [batchId, setBatchId] = useWorkspaceDraft("strategy-batch", "");
  const [selected, setSelected] = useState<string[]>([]);
  const batch = useStrategyBatch(batchId, refreshRecentRuns);
  const [saved, setSaved] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const submitted = useActionError();
  const reportActed = useActionError();
  const { clear: clearSubmitted } = submitted;
  const { clear: clearReportActed } = reportActed;
  useEffect(() => {
    clearSubmitted();
  }, [view, chosen, form, clearSubmitted]);
  useEffect(() => {
    clearReportActed();
  }, [report?.id, clearReportActed]);

  const submittedGap = parseDataGap(submitted.error, { minutes: form.minutes !== "0" });
  const submittedAction = submittedGap
    ? {
        label: t("去采集"),
        onClick: () =>
          context.navigate("workspace.data-dataset", { params: dataGapParams(submittedGap) }),
      }
    : undefined;
  const openDataset = (params: Record<string, string>) =>
    context.navigate("workspace.data-dataset", { params });
  const library = view === "library";
  const strategy = strategies?.strategies.find(item => item.id === chosen);
  const enteredSymbols = symbolsOf(form.symbols);
  const effectiveMode = form.multiSymbolMode ?? (strategy?.portfolio ? "portfolio" : "separate");
  const separateMulti = !library && enteredSymbols.length > 1 && effectiveMode === "separate";
  const catalog = useStoredCatalog(context, form.minutes !== "0");
  const [beginPlaceholder, endPlaceholder] = datePlaceholders(
    catalog,
    enteredSymbols,
    separateMulti,
  );
  const [scopeBegin, scopeEnd] = datePlaceholders(catalog, enteredSymbols, false);
  const inScope = enteredSymbols.length
    ? (run: RunSummary) => onScope(form, run, scopeBegin, scopeEnd)
    : undefined;
  const activeValidation =
    form.validation === "rolling" && !separateMulti
      ? "rolling"
      : form.holdoutDay
        ? "holdout"
        : "none";

  const change = (patch: Partial<RunForm>) => setForm(previous => ({ ...previous, ...patch }));
  const submit = async () => {
    if (!strategy) return;
    setBusy(true);
    setSaved([]);
    await submitted.attempt(async () => {
      const written = await source.save();
      setSaved(written);
      if (written.length) await readStrategies();
      const id = identity(strategy.id);
      const { run, swept } = runRequest(
        { ...form, validation: activeValidation },
        repository,
        strategy.id,
        strategy.parameters,
        id,
      );
      if (activeValidation === "rolling" && !swept.length)
        throw new Error(t("滚动验证需要至少一个参数填多个值"));
      if (separateMulti || swept.length || activeValidation === "rolling") {
        const searchMode = !separateMulti && swept.length ? (form.sweepMode ?? "grid") : "grid";
        await lab({
          case: "startSweep",
          value: {
            id,
            base: run,
            symbols: separateMulti ? run.symbols : [],
            values: swept,
            processes: Number(form.processes) || 0,
            searchMode: searchMode === "grid" ? "" : searchMode,
            maxTrials: searchMode === "grid" ? 0 : Math.max(2, Number(form.maxTrials) || 24),
            walkForward:
              activeValidation === "rolling"
                ? {
                    trainingDays: Number(form.trainingDays),
                    validationDays: Number(form.validationDays),
                    embargoDays: Math.max(0, Number(form.embargoDays) || 0),
                    objective: form.rollingObjective ?? "sharpe",
                  }
                : undefined,
          },
        });
        setReport({ kind: "sweep", id });
      } else {
        await lab({ case: "startBacktest", value: run });
        setReport({ kind: "run", id });
      }
      setTab(previous => (previous === "split" ? "split" : "result"));
      void readRecentRuns().catch(() => {});
    });
    setBusy(false);
  };

  const cancelReport = () =>
    report &&
    reportActed.attempt(async () => {
      await (report.kind === "sweep"
        ? lab({ case: "cancelSweep", value: { id: report.id } })
        : lab({ case: "cancelRun", value: { id: report.id } }));
      await refreshReport();
      void readRecentRuns().catch(() => {});
    });

  const field = (
    label: string,
    key: keyof RunForm,
    props: Record<string, string> = {},
    className?: string,
  ) => (
    <label className={className}>
      <span>{t(label)}</span>
      <input
        {...props}
        value={form[key] as string}
        onChange={event => change({ [key]: event.target.value } as Partial<RunForm>)}
      />
    </label>
  );
  const [folded, setFolded] = useConfigFold();
  const menu = useModuleMenu({
    repository,
    source,
    deleted: path => {
      forgetStrategy(path);
      const stem = path.replace(/^strategies\//, "").replace(/\.py$/, "");
      if (chosen.startsWith(`${stem}.`)) {
        setChosen("");
        setReport(null);
      }
      setView("library");
      void readStrategies();
    },
  });
  const activeSelected = selected.filter(id => strategies?.strategies.some(item => item.id === id));
  const startBatch = async () => {
    const targets = (strategies?.strategies ?? []).filter(item => activeSelected.includes(item.id));
    setBusy(true);
    setSaved([]);
    await submitted.attempt(async () => {
      const written = await source.save();
      setSaved(written);
      if (written.length) await readStrategies();
      const id = identity("batch");
      const settings: RunForm = {
        ...form,
        params: {},
        validation: form.holdoutDay ? "holdout" : "none",
      };
      const members = targets.map(
        entry => runRequest(settings, repository, entry.id, entry.parameters, id).run,
      );
      await lab({
        case: "startSweep",
        value: {
          id,
          base: members[0],
          strategies: members.map(run => ({ strategy: run.strategy, params: run.params })),
          processes: Number(form.processes) || 0,
        },
      });
      setBatchId(id);
      setSelected([]);
    });
    setBusy(false);
  };
  const startSlippageStress = (runId: string) =>
    void reportActed.attempt(async () => {
      const detail = await lab({ case: "getRun", value: { id: runId } });
      const sweepId = identity(detail.summary?.strategy || "stress", "slip");
      const base = slippageStressBase(detail, repository);
      await lab({
        case: "startSweep",
        value: {
          id: sweepId,
          base,
          slippageTicks: [...SLIPPAGE_STRESS_TICKS],
          processes: Number(form.processes) || 0,
        },
      });
      setReport({ kind: "sweep", id: sweepId });
      setTab("result");
    });

  const annotations = useAnnotations();
  const singleRuns = runs.filter(run => !run.sweepId || run.id !== `${run.sweepId}-rolling`);
  const strategyRuns = strategy ? singleRuns.filter(run => run.strategy === strategy.id) : [];
  const previousRun = (() => {
    if (report?.kind !== "run" || !strategy) return undefined;
    const idx = strategyRuns.findIndex(run => run.id === report.id);
    if (idx < 0) return undefined;
    return strategyRuns.slice(idx + 1).find(run => runState(run.state) === "succeeded");
  })();
  // The open strategy's result follows the settings in use.
  useEffect(() => {
    if (shownScope.current === scopeText) return;
    shownScope.current = scopeText;
    if (library || !chosen) return;
    const { run, failed } = strategyResult(singleRuns, chosen, inScope);
    const current = failed ?? (run && inScope?.(run) ? run : undefined);
    setReport(current ? { kind: "run", id: current.id } : null);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scopeText]);
  const items: ModuleItem[] = (strategies?.strategies ?? []).map(item => {
    const [module, name] = item.id.split(".");
    return {
      id: item.id,
      name,
      module,
      group: t(item.portfolio ? "组合" : "单品种"),
      description: item.description.replace(/`/g, ""),
    };
  });
  const shownRun = (id: string) => {
    const { run, failed } = strategyResult(singleRuns, id, inScope);
    return (failed ?? run)?.id;
  };
  const choose = (item: ModuleItem, run = shownRun(item.id)) => {
    const entry = strategies!.strategies.find(entry => entry.id === item.id)!;
    setChosen(item.id);
    setReport(run ? { kind: "run", id: run } : null);
    setTab(run ? "result" : "code");
    setView("detail");
    source.open(`strategies/${item.module}.py`);
    if (item.id !== chosen) {
      change({
        multiSymbolMode: undefined,
        params: Object.fromEntries(entry.parameters.map(p => [p.name, defaultText(p)])),
      });
    }
  };
  const openCode = (path: string, id = "") => {
    setChosen(id);
    setReport(null);
    setTab("code");
    setView("detail");
    source.open(path);
  };
  const activeState =
    report?.kind === "run" && activeRun?.id === report.id ? runState(activeRun.state) : "";
  const reportRunning =
    report?.kind === "run"
      ? activeState === "running"
      : report?.kind === "sweep" && activeSweep?.id === report.id && activeSweep.active;
  const shownReport = tab !== "code" ? report : null;
  // The strategy is named by the switch beside the run.
  const shortName = (run: RunSummary) =>
    run.label || runName(run).replace(`${chosen.split(".")[1]} `, "");
  const sameName = (item: ModuleItem) => items.filter(other => other.name === item.name).length > 1;
  const onRunShortcut = (event: KeyboardEvent) => {
    if (
      !library &&
      strategy &&
      !busy &&
      !context.busy &&
      (event.metaKey || event.ctrlKey) &&
      event.key === "Enter"
    ) {
      event.preventDefault();
      void submit();
    }
  };

  return (
    <section
      className="lab-research"
      data-folded={(!library && folded) || undefined}
      onKeyDown={onRunShortcut}
    >
      <RunScope
        context={context}
        form={form}
        begin={beginPlaceholder}
        end={endPlaceholder}
        conditions={conditions}
        catalog={catalog}
      />
      {!library && (
        <header className="lab-report-header lab-page-head">
          <button type="button" className="lab-link" onClick={() => setView("library")}>
            {t("← 策略库")}
          </button>
          {strategy ? (
            <select
              className="lab-strategy-switch"
              aria-label={t("切换策略")}
              value={chosen}
              onChange={event => choose(items.find(item => item.id === event.target.value)!)}
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
          {strategy && (
            <div className="lab-segmented" role="group" aria-label={t("策略")}>
              {(
                [
                  ["result", "回测"],
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
          {shownReport?.kind === "run" && shownReport.fromSweep && (
            <button
              type="button"
              className="lab-link"
              onClick={() => setReport({ kind: "sweep", id: shownReport.fromSweep! })}
            >
              {t("← 扫描")}
            </button>
          )}
          {shownReport && (
            <h2>
              {shownReport.kind === "sweep"
                ? t(
                    activeSweep?.id === shownReport.id && activeSweep.runs[0]?.params[slippageKey]
                      ? "滑点压力测试 {id}"
                      : activeSweep?.id === shownReport.id &&
                          new Set(activeSweep.runs.map(run => run.symbols.join(","))).size > 1 &&
                          Object.keys(activeSweep.runs[0]?.params ?? {}).length === 0
                        ? "多品种回测 {id}"
                        : "扫描 {id}",
                    {
                      id: recordName(
                        shownReport.id,
                        activeSweep?.id === shownReport.id
                          ? activeSweep.runs[0]?.strategy.split(".")[1]
                          : undefined,
                      ),
                    },
                  )
                : activeRun?.id === shownReport.id
                  ? shortName(activeRun)
                  : recordName(shownReport.id)}
            </h2>
          )}
          {shownReport && activeState && (
            <StatusBadge tone={runTones[activeState] ?? "neutral"}>
              {states[activeState] ? t(states[activeState]!) : activeState}
            </StatusBadge>
          )}
          {shownReport?.kind === "run" &&
            !shownReport.fromSweep &&
            activeRun?.id === shownReport.id &&
            inScope &&
            !inScope(activeRun) && <StatusBadge tone="neutral">{t("条件不同")}</StatusBadge>}
          <span className="lab-spacer" />
          {shownReport && reportRunning && (
            <button type="button" className="lab-link" onClick={() => void cancelReport()}>
              {t("取消")}
            </button>
          )}
          {shownReport && (
            <button
              type="button"
              className="lab-link"
              onClick={() =>
                context.navigate("workspace.lab-runs", {
                  params:
                    shownReport.kind === "sweep"
                      ? { sweep: shownReport.id }
                      : { run: shownReport.id },
                })
              }
            >
              {t("在回测中打开")}
            </button>
          )}
          {shownReport?.kind === "run" &&
            activeState === "succeeded" &&
            (!activeRun?.sweepId || shownReport.id !== `${activeRun.sweepId}-rolling`) && (
              <>
                <button
                  type="button"
                  className="lab-link"
                  onClick={() => startSlippageStress(shownReport.id)}
                >
                  {t("滑点压力测试")}
                </button>
                <button
                  type="button"
                  onClick={() =>
                    context.navigate("workspace.trading-strategies", {
                      params: { run: shownReport.id },
                    })
                  }
                >
                  {t("去部署")}
                </button>
              </>
            )}
        </header>
      )}
      {menu.overlay}
      {menu.copying && (
        <NewModule
          key={menu.copying.id}
          hideTrigger
          folder="strategies"
          repository={repository}
          copying={menu.copying}
          cancelCopy={menu.clearCopy}
          created={(path, id) => {
            openCode(path, id);
            void readStrategies();
          }}
        />
      )}
      {library ? (
        <StrategyLibrary
          items={items}
          runs={singleRuns}
          chosen={chosen}
          selected={activeSelected}
          setSelected={setSelected}
          batch={batch}
          onScope={inScope}
          search={search}
          setSearch={setSearch}
          busy={busy || Boolean(context.busy)}
          startBatch={() => void startBatch()}
          compareRuns={ids =>
            context.navigate("workspace.lab-runs", { params: { compare: ids.join(",") } })
          }
          cancelBatch={() =>
            void submitted.attempt(async () => {
              await lab({ case: "cancelSweep", value: { id: batchId } });
            })
          }
          open={choose}
          contextTarget={menu.target}
          onMenu={menu.open}
          newModule={
            <NewModule
              folder="strategies"
              repository={repository}
              created={(path, id) => {
                openCode(path, id);
                void readStrategies();
              }}
            />
          }
          notices={
            <>
              <ActionError
                error={submitted.error}
                namespace="asterion.terminal.research"
                onDismiss={submitted.clear}
                action={submittedAction}
              />
              <ModuleErrors
                folder="strategies"
                modules={strategies}
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
              modules={strategies}
              focus={chosen}
              changed={() => void readStrategies()}
              onDuplicate={source.path.startsWith("strategies/") ? menu.copy : undefined}
              onDelete={path => menu.remove(path)}
            />
          )}
          {(tab === "result" || tab === "split") &&
            (report ? (
              <div className="lab-center lab-runs-view">
                <ActionError
                  error={reportActed.error}
                  namespace="asterion.terminal.research"
                  onDismiss={reportActed.clear}
                />
                {report.kind === "run" ? (
                  <RunResult
                    key={report.id}
                    id={report.id}
                    state={activeState}
                    previous={previousRun}
                    openVersion={version =>
                      context.navigate("workspace.data-dataset", { params: { version } })
                    }
                    openDataset={openDataset}
                    openFactor={factorId =>
                      context.navigate("workspace.lab-factors", {
                        params: { factorResult: factorId },
                      })
                    }
                    openRun={runId =>
                      context.navigate("workspace.lab-runs", { params: { run: runId } })
                    }
                  />
                ) : activeSweep?.id === report.id ? (
                  <SweepResult
                    runs={activeSweep.runs}
                    selected={activeSweep.selected}
                    pbo={activeSweep.pbo}
                    pboRankCorr={activeSweep.pboRankCorr}
                    open={id => setReport({ kind: "run", id, fromSweep: report.id })}
                    rolling={
                      activeSweep.rolling
                        ? () =>
                            setReport({
                              kind: "run",
                              id: activeSweep.rolling!.id,
                              fromSweep: report.id,
                            })
                        : undefined
                    }
                  />
                ) : (
                  <p className="subtle">{t("正在读取结果…")}</p>
                )}
              </div>
            ) : (
              <div className="lab-placeholder">
                <div>
                  <p>{t("还没有回测")}</p>
                  <p className="subtle">{t("按 ⌘Enter / Ctrl+Enter 或点击右侧「运行」发起回测")}</p>
                </div>
              </div>
            ))}
        </div>
      )}
      {strategy && !library && (
        <aside className="lab-config" aria-label={t("参数")}>
          <h2>
            {!folded && t("参数")}
            <ConfigFold folded={folded} fold={setFolded} />
          </h2>
          {!folded && (
            <>
              <div className="lab-form lab-parameters">
                {strategy.parameters.map(parameter => (
                  <label
                    key={parameter.name}
                    className={strategy.parameters.length === 1 ? "wide" : undefined}
                  >
                    <span>{parameter.name}</span>
                    <input
                      className="lab-param"
                      placeholder={defaultText(parameter)}
                      title={t("多个值用逗号分隔即为扫描")}
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
                {strategy.parameters.length > 0 && (
                  <p className="subtle wide lab-param-hint">{t("多值用逗号分隔即扫描")}</p>
                )}
                {enteredSymbols.length > 1 && (
                  <fieldset className="wide lab-segmented">
                    <legend className="sr-only">{t("多品种运行方式")}</legend>
                    {(["separate", "portfolio"] as const).map(item => (
                      <label key={item}>
                        <input
                          type="radio"
                          name={modeGroup}
                          checked={effectiveMode === item}
                          onChange={() => change({ multiSymbolMode: item })}
                        />
                        {t(item === "separate" ? "分别回测" : "组合合跑")}
                      </label>
                    ))}
                  </fieldset>
                )}
              </div>
              {(() => {
                const sweepCount = strategy.parameters.reduce((total, parameter) => {
                  const parts = (form.params[parameter.name] ?? "")
                    .split(",")
                    .map(part => part.trim())
                    .filter(Boolean);
                  return total * Math.max(1, parts.length);
                }, 1);
                const sweepMode = form.sweepMode ?? "grid";
                const effectiveTrials =
                  !separateMulti && sweepCount > 1 && sweepMode !== "grid"
                    ? Math.min(sweepCount, Math.max(2, Number(form.maxTrials) || 24))
                    : sweepCount;
                return (
                  <>
                    <Section title={t("扫描与验证")}>
                      {!separateMulti && sweepCount > 1 && (
                        <>
                          <label className="wide">
                            <span>{t("搜索方式")}</span>
                            <select
                              value={sweepMode}
                              onChange={event =>
                                change({
                                  sweepMode: event.target.value as RunForm["sweepMode"],
                                })
                              }
                            >
                              <option value="grid">{t("网格枚举")}</option>
                              <option value="lhs">{t("拉丁超立方 LHS")}</option>
                              <option value="bayes">{t("贝叶斯优化 GP")}</option>
                            </select>
                          </label>
                          {sweepMode !== "grid" &&
                            field("最大采样组数", "maxTrials", { placeholder: "24" }, "wide")}
                        </>
                      )}
                      {!separateMulti && (
                        <label className="lab-check wide">
                          <input
                            type="checkbox"
                            checked={activeValidation === "rolling"}
                            onChange={event =>
                              change({ validation: event.target.checked ? "rolling" : "none" })
                            }
                          />
                          {t("滚动验证")}
                        </label>
                      )}
                      {activeValidation === "rolling" && (
                        <>
                          {field("训练交易日数", "trainingDays")}
                          {field("每轮验证交易日数", "validationDays")}
                          {field("隔离带（交易日）", "embargoDays", { placeholder: "0" })}
                          <label>
                            <span>{t("选参目标")}</span>
                            <select
                              value={form.rollingObjective ?? "sharpe"}
                              onChange={event =>
                                change({
                                  rollingObjective: event.target
                                    .value as RunForm["rollingObjective"],
                                })
                              }
                            >
                              <option value="sharpe">{t("夏普比率")}</option>
                              <option value="dsr">{t("DSR 置信度")}</option>
                              <option value="calmar">{t("卡玛比率")}</option>
                              <option value="total_return">{t("总收益")}</option>
                            </select>
                          </label>
                        </>
                      )}
                      {field(
                        "并行进程数",
                        "processes",
                        { placeholder: defaultProcesses() },
                        "wide",
                      )}
                    </Section>
                    <RecentRuns
                      runs={strategyRuns}
                      activeId={report?.kind === "run" ? report.id : ""}
                      annotations={annotations}
                      open={id => {
                        setReport({ kind: "run", id });
                        setTab(previous => (previous === "split" ? "split" : "result"));
                      }}
                      openAll={() => context.navigate("workspace.lab-runs", {})}
                    />
                    <footer className="lab-submit">
                      <button
                        type="button"
                        className="primary"
                        disabled={busy || context.busy}
                        onClick={() => void submit()}
                      >
                        {effectiveTrials > 1
                          ? t("运行扫描（{count} 组）", { count: effectiveTrials })
                          : t("运行")}
                      </button>
                      {saved.length > 0 && (
                        <p className="subtle">
                          {t("已保存 {files}", { files: saved.join(listSeparator()) })}
                        </p>
                      )}
                      <ActionError
                        error={submitted.error}
                        namespace="asterion.terminal.research"
                        onDismiss={submitted.clear}
                        action={submittedAction}
                      />
                    </footer>
                  </>
                );
              })()}
            </>
          )}
        </aside>
      )}
    </section>
  );
}

// The strategy's last runs, each opening its report in place.
function RecentRuns({
  runs,
  activeId,
  annotations,
  open,
  openAll,
}: {
  runs: RunSummary[];
  activeId: string;
  annotations: Record<string, { starred?: boolean; note?: string }>;
  open: (id: string) => void;
  openAll: () => void;
}) {
  if (!runs.length) return null;
  return (
    <section className="lab-recent" aria-label={t("最近运行")}>
      <div className="lab-recent-header">
        <h3>{t("最近运行")}</h3>
        <button type="button" className="lab-link" onClick={openAll}>
          {t("全部回测记录")}
        </button>
      </div>
      {runs.slice(0, 6).map(run => {
        const ann = annotations[run.id];
        return (
          <button
            key={run.id}
            type="button"
            className="lab-link"
            aria-pressed={activeId === run.id}
            title={ann?.note || runName(run)}
            onClick={() => open(run.id)}
          >
            <span>
              {ann?.starred && <span className="lab-star-mark">★ </span>}
              {runName(run)}
            </span>
            <span className={signed(run.metrics.total_return)}>
              {metric(run.metrics.total_return, true)}
            </span>
          </button>
        );
      })}
    </section>
  );
}
