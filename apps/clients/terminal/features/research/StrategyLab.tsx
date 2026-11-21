import { DateInput } from "../../src/ui/DateInput";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { useCallback, useEffect, useId, useState } from "react";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { lab } from "../../src/bridge/lab";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { RunSummary } from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { useLabActiveReport } from "../../src/host/lab";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import {
  defaultForm,
  defaultProcesses,
  defaultText,
  formFromRun,
  identity,
  recordName,
  runRequest,
  runState,
  symbolsOf,
  type RunForm,
} from "./research-lab";
import {
  ConfigFold,
  datePlaceholders,
  LabCentre,
  ModuleErrors,
  ModuleList,
  NewModule,
  Section,
  SymbolField,
  useConfigFold,
  useModuleMenu,
  useModules,
  useSource,
  useStoredCatalog,
} from "./LabParts";
import { RunResult, SweepResult, metric, runTones, signed, states } from "./ResearchResult";
import { StrategyLifecycle } from "./StrategyLifecycle";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

type ActiveReport = { kind: "run" | "sweep"; id: string; fromSweep?: string } | null;

// The strategy view: the strategy modules, their code or the latest run/sweep
// report in place, and the configuration beside it for immediate iteration.
export function StrategyLab(context: TerminalContext) {
  const modeGroup = useId();
  const validationGroup = useId();
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
  const [form, setForm] = useWorkspaceDraft<RunForm>("run-form", defaultForm);
  const source = useSource(repository);
  const [runs, setRuns] = useState<RunSummary[]>([]);
  const readRecentRuns = useCallback(async () => {
    const reply = await lab({ case: "listRecentRuns", value: {} });
    setRuns(reply.runs);
  }, []);
  useEffect(() => {
    void readRecentRuns().catch(notify);
  }, [readRecentRuns]);

  const loadRunId = context.workspaceParams?.loadRun;
  useEffect(() => {
    if (!chosen || !strategies || strategies.strategies.some(item => item.id === chosen)) return;
    const [stem] = chosen.split(".");
    const renamed = strategies.strategies.find(item => item.id.startsWith(`${stem}.`));
    if (renamed) setChosen(renamed.id);
  }, [chosen, strategies, setChosen]);

  useEffect(() => {
    if (loadRunId || chosen || source.path || !strategies?.strategies.length) return;
    const first = strategies.strategies[0]!;
    setChosen(first.id);
    source.open(`strategies/${first.id.split(".")[0]}.py`);
    setForm(previous => ({
      ...previous,
      params: Object.fromEntries(
        first.parameters.map(p => [p.name, previous.params[p.name] ?? defaultText(p)]),
      ),
    }));
  }, [loadRunId, chosen, source, strategies, setChosen, setForm]);

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
        setReport(null);
        setForm(loaded.form);
        context.navigate("workspace.lab-strategies", {});
      })
      .catch(failure => active && notify(failure));
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

  const [saved, setSaved] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const submitted = useActionError();
  const reportActed = useActionError();

  const strategy = strategies?.strategies.find(item => item.id === chosen);
  const enteredSymbols = symbolsOf(form.symbols);
  const effectiveMode = form.multiSymbolMode ?? (strategy?.portfolio ? "portfolio" : "separate");
  const separateMulti = enteredSymbols.length > 1 && effectiveMode === "separate";
  const catalog = useStoredCatalog(context, form.minutes !== "0");
  const [beginPlaceholder, endPlaceholder] = datePlaceholders(
    catalog,
    enteredSymbols,
    separateMulti,
  );
  const activeValidation =
    separateMulti && form.validation === "rolling" ? "none" : form.validation;

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
        await lab({
          case: "startSweep",
          value: {
            id,
            base: run,
            symbols: separateMulti ? run.symbols : [],
            values: swept,
            processes: Number(form.processes) || 0,
            walkForward:
              activeValidation === "rolling"
                ? {
                    trainingDays: Number(form.trainingDays),
                    validationDays: Number(form.validationDays),
                  }
                : undefined,
          },
        });
        setReport({ kind: "sweep", id });
      } else {
        await lab({ case: "startBacktest", value: run });
        setReport({ kind: "run", id });
      }
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
  const dateField = (label: string, key: "begin" | "end" | "holdoutDay", placeholder?: string) => (
    <label>
      <span>{t(label)}</span>
      <DateInput
        value={form[key]}
        placeholder={placeholder}
        onChange={value => change({ [key]: value })}
      />
    </label>
  );
  const validation = { none: "不验证", holdout: "留出", rolling: "滚动验证" } as const;
  const [folded, setFolded] = useConfigFold();
  const strategyPath = `strategies/${strategy?.id.split(".")[0]}.py`;
  const menu = useModuleMenu({
    repository,
    source,
    deleted: path => {
      forgetStrategy(path);
      const stem = path.replace(/^strategies\//, "").replace(/\.py$/, "");
      if (chosen.startsWith(`${stem}.`)) setChosen("");
      void readStrategies();
    },
  });
  const activeState =
    report?.kind === "run" && activeRun?.id === report.id ? runState(activeRun.state) : "";
  const reportRunning =
    report?.kind === "run"
      ? activeState === "running"
      : report?.kind === "sweep" && activeSweep?.id === report.id && activeSweep.active;

  return (
    <section className="lab-research" data-folded={folded || undefined}>
      <aside className="lab-modules" aria-label={t("策略")}>
        <h2 className="sr-only">{t("策略")}</h2>
        <ModuleList
          folder="strategies"
          chosen={chosen}
          contextTarget={menu.target}
          items={(strategies?.strategies ?? []).map(item => {
            const [module, name] = item.id.split(".");
            const latest = runs.find(
              run =>
                run.strategy === item.id && (!run.sweepId || run.id !== `${run.sweepId}-rolling`),
            );
            const value = latest?.metrics.total_return;
            return {
              id: item.id,
              name,
              module,
              group: t(item.portfolio ? "组合" : "单品种"),
              description: item.description.replace(/`/g, ""),
              figure:
                value === undefined
                  ? undefined
                  : {
                      text: metric(value, true),
                      className: signed(value),
                      title: t("最近一次总收益"),
                    },
            };
          })}
          choose={item => {
            const entry = strategies!.strategies.find(entry => entry.id === item.id)!;
            setChosen(item.id);
            setReport(null);
            source.open(`strategies/${item.module}.py`);
            if (item.id !== chosen) {
              change({
                multiSymbolMode: undefined,
                params: Object.fromEntries(entry.parameters.map(p => [p.name, defaultText(p)])),
              });
            }
          }}
          onMenu={menu.open}
        />
        <NewModule
          key={menu.copying?.id ?? "new"}
          folder="strategies"
          repository={repository}
          copying={menu.copying}
          cancelCopy={menu.clearCopy}
          created={(path, id) => {
            setReport(null);
            if (id) setChosen(id);
            source.open(path);
            void readStrategies();
          }}
        />
        <ModuleErrors
          folder="strategies"
          modules={strategies}
          contextTarget={menu.target}
          open={path => {
            setReport(null);
            source.open(path);
          }}
          onMenu={menu.open}
        />
        {menu.overlay}
      </aside>
      {strategy && (
        <StrategyLifecycle
          strategy={strategy.id}
          path={strategyPath}
          unsaved={source.drafts[strategyPath] !== undefined}
          runs={runs.filter(run => !run.sweepId || run.id !== `${run.sweepId}-rolling`)}
          openCode={() => {
            setReport(null);
            source.open(strategyPath);
          }}
          backtest={() => {
            setFolded(false);
            if (enteredSymbols.length) void submit();
          }}
          openRun={id => setReport({ kind: "run", id })}
          deploy={run => context.navigate("workspace.trading-strategies", { params: { run } })}
          openTrading={account =>
            context.navigate("workspace.trading-strategies", account ? { params: { account } } : {})
          }
        />
      )}
      {report ? (
        <div className="lab-center lab-runs-view">
          <header className="lab-report-header">
            <button type="button" className="lab-link" onClick={() => setReport(null)}>
              {t("← 源码")}
            </button>
            {report.kind === "run" && report.fromSweep && (
              <button
                type="button"
                className="lab-link"
                onClick={() => setReport({ kind: "sweep", id: report.fromSweep! })}
              >
                {t("← 扫描")}
              </button>
            )}
            <h2>
              {report.kind === "sweep"
                ? t(
                    activeSweep?.id === report.id &&
                      new Set(activeSweep.runs.map(run => run.symbols.join(","))).size > 1 &&
                      Object.keys(activeSweep.runs[0]?.params ?? {}).length === 0
                      ? "多品种回测 {id}"
                      : "扫描 {id}",
                    { id: recordName(report.id) },
                  )
                : activeRun?.id === report.id && activeRun.label
                  ? activeRun.label
                  : recordName(report.id)}
            </h2>
            {activeState && (
              <StatusBadge tone={runTones[activeState] ?? "neutral"}>
                {states[activeState] ? t(states[activeState]!) : activeState}
              </StatusBadge>
            )}
            <span className="lab-spacer" />
            {reportRunning && (
              <button type="button" className="lab-link" onClick={() => void cancelReport()}>
                {t("取消")}
              </button>
            )}
            <button
              type="button"
              className="lab-link"
              onClick={() =>
                context.navigate("workspace.lab-runs", {
                  params: report.kind === "sweep" ? { sweep: report.id } : { run: report.id },
                })
              }
            >
              {t("在回测中打开")}
            </button>
            {report.kind === "run" &&
              activeState === "succeeded" &&
              (!activeRun?.sweepId || report.id !== `${activeRun.sweepId}-rolling`) && (
                <button
                  type="button"
                  className="lab-link"
                  onClick={() =>
                    context.navigate("workspace.trading-strategies", {
                      params: { run: report.id },
                    })
                  }
                >
                  {t("去部署")}
                </button>
              )}
          </header>
          <ActionError error={reportActed.error} namespace="asterion.terminal.research" />
          {report.kind === "run" ? (
            <RunResult
              key={report.id}
              id={report.id}
              state={activeState}
              openVersion={version =>
                context.navigate("workspace.data-dataset", { params: { version } })
              }
            />
          ) : activeSweep?.id === report.id ? (
            <SweepResult
              runs={activeSweep.runs}
              selected={activeSweep.selected}
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
        <LabCentre
          source={source}
          repository={repository}
          modules={strategies}
          focus={chosen}
          changed={() => void readStrategies()}
          onDelete={path => menu.remove(path)}
        />
      )}
      {strategy && (
        <aside className="lab-config" aria-label={t("运行配置")}>
          <h2>
            {!folded && t("运行配置")}
            <ConfigFold folded={folded} fold={setFolded} />
          </h2>
          {!folded && (
            <>
              <Section title={t("品种与区间")} open>
                <SymbolField
                  context={context}
                  value={form.symbols}
                  minutes={form.minutes !== "0"}
                  catalog={catalog}
                  onChange={symbols => change({ symbols })}
                />
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
                {dateField("开始日", "begin", beginPlaceholder)}
                {dateField("结束日", "end", endPlaceholder)}
                <label className="wide">
                  <span>{t("周期")}</span>
                  <select
                    value={form.minutes}
                    onChange={event => change({ minutes: event.target.value })}
                  >
                    <option value="0">{t("日线")}</option>
                    {[1, 5, 10, 15, 30, 60].map(value => (
                      <option key={value} value={String(value)}>
                        {t("{count} 分钟", { count: value })}
                      </option>
                    ))}
                  </select>
                </label>
              </Section>
              <Section title={t("参数")} open>
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
              </Section>
              <Section title={t("资金与风控")}>
                {field("初始资金", "deposit")}
                {field("滑点（跳）", "slippage")}
                {field("单笔手数", "maxOrderLots")}
                {field("单合约持仓手数", "maxContractLots")}
                {field("账户总持仓手数", "maxGrossLots")}
                {field("在途委托数", "maxWorkingOrders")}
                {field("总权重上限", "maxGrossWeight")}
                {field("净权重上限", "maxNetWeight")}
                {field("保证金率上限", "maxMarginRatio")}
                {field("最小调仓偏离", "minRebalanceWeight")}
                {field("每年期数", "annualPeriods")}
                <label className="lab-check wide">
                  <input
                    type="checkbox"
                    checked={form.zeroRiskFree}
                    onChange={event => change({ zeroRiskFree: event.target.checked })}
                  />
                  {t("无风险利率为零")}
                </label>
              </Section>
              <Section title={t("扫描与验证")}>
                <fieldset className="wide lab-segmented">
                  <legend className="sr-only">{t("扫描与验证")}</legend>
                  {(separateMulti
                    ? (["none", "holdout"] as const)
                    : (["none", "holdout", "rolling"] as const)
                  ).map(kind => (
                    <label key={kind}>
                      <input
                        type="radio"
                        name={validationGroup}
                        checked={activeValidation === kind}
                        onChange={() => change({ validation: kind })}
                      />
                      {t(validation[kind])}
                    </label>
                  ))}
                </fieldset>
                {activeValidation === "holdout" && dateField("留出日", "holdoutDay")}
                {activeValidation === "rolling" && (
                  <>
                    {field("训练交易日数", "trainingDays")}
                    {field("每轮验证交易日数", "validationDays")}
                  </>
                )}
                {field(
                  "并行进程数",
                  "processes",
                  { placeholder: defaultProcesses() },
                  activeValidation === "holdout" ? undefined : "wide",
                )}
              </Section>
              <footer className="lab-submit">
                <button
                  className="primary"
                  disabled={busy || context.busy}
                  onClick={() => void submit()}
                >
                  {t("运行")}
                </button>
                {saved.length > 0 && (
                  <p className="subtle">
                    {t("已保存 {files}", { files: saved.join(listSeparator()) })}
                  </p>
                )}
                <ActionError error={submitted.error} namespace="asterion.terminal.research" />
                <RecentRuns
                  runs={runs.filter(run => run.strategy === strategy.id && !run.sweepId)}
                  open={id => setReport({ kind: "run", id })}
                  openAll={() => context.navigate("workspace.lab-runs", {})}
                />
              </footer>
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
  open,
  openAll,
}: {
  runs: RunSummary[];
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
      {runs.slice(0, 3).map(run => (
        <button key={run.id} type="button" className="lab-link" onClick={() => open(run.id)}>
          <span>{run.label || recordName(run.id)}</span>
          <span className={signed(run.metrics.total_return)}>
            {metric(run.metrics.total_return, true)}
          </span>
        </button>
      ))}
    </section>
  );
}
