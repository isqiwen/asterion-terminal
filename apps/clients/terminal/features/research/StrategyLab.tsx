import { DateInput } from "../../src/ui/DateInput";
import { useCallback, useEffect, useMemo, useState } from "react";
import { lab } from "../../src/bridge/lab";
import type { RunSummary } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import {
  defaultForm,
  defaultText,
  identity,
  parameterKind,
  runState,
  runRequest,
  type RunForm,
} from "./research-lab";
import {
  ConfigFold,
  DeleteRecord,
  LabCentre,
  LabHeader,
  ModuleErrors,
  NewModule,
  useConfigFold,
  useModules,
  useSource,
} from "./LabParts";
import { StatusBadge } from "../../src/ui/StatusBadge";
import {
  RunComparison,
  RunResult,
  SweepResult,
  metric,
  runTones,
  signed,
  states,
} from "./ResearchResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

type Shown =
  { kind: "run"; id: string } | { kind: "sweep"; id: string } | { kind: "compare" } | null;

// The strategy view: the strategy modules, a run's configuration, its result
// and every run so far, all on the lab service.
export function StrategyLab(context: TerminalContext) {
  const repository = context.snapshot?.research_directory ?? "";
  const { modules: strategies, read: readStrategies } = useModules(repository);
  const [chosen, setChosen] = useState("");
  const [form, setForm] = useState<RunForm>(defaultForm);
  const [runs, setRuns] = useState<RunSummary[]>([]);
  const [sweep, setSweep] = useState<{
    id: string;
    runs: RunSummary[];
    selected: string;
    active: boolean;
  } | null>(null);
  const [shown, setShown] = useState<Shown>(null);
  const source = useSource(repository, shown);
  const [saved, setSaved] = useState<string[]>([]);
  const [compared, setCompared] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const submitted = useActionError();
  const cancelled = useActionError();

  const readRuns = useCallback(async () => {
    try {
      setRuns((await lab({ case: "listRuns", value: {} })).runs);
    } catch (failure) {
      notify(failure);
    }
  }, []);
  useEffect(() => void readRuns(), [readRuns]);
  // Read again while something runs, or the sweep on show has runs to come.
  const running =
    runs.some(run => runState(run.state) === "running") ||
    (shown?.kind === "sweep" && sweep?.id === shown.id && sweep.active);
  useEffect(() => {
    if (!running) return;
    const timer = setInterval(() => void readRuns(), 2000);
    return () => clearInterval(timer);
  }, [running, readRuns]);
  useEffect(() => {
    if (shown?.kind !== "sweep") return;
    let active = true;
    void lab({ case: "getSweep", value: { id: shown.id } })
      .then(reply => {
        if (!active) return;
        setSweep({
          id: shown.id,
          runs: [...reply.runs],
          selected: reply.selectedId,
          active: reply.active,
        });
      })
      .catch(failure => active && notify(failure));
    return () => {
      active = false;
    };
  }, [shown, runs]);

  const strategy = strategies?.strategies.find(item => item.id === chosen);
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
      const { run, swept } = runRequest(form, repository, strategy.id, strategy.parameters, id);
      if (form.validation === "rolling" && !swept.length)
        throw new Error(t("滚动验证需要至少一个参数填多个值"));
      if (swept.length || form.validation === "rolling") {
        await lab({
          case: "startSweep",
          value: {
            id,
            base: run,
            values: swept,
            processes: Number(form.processes) || 0,
            walkForward:
              form.validation === "rolling"
                ? {
                    trainingDays: Number(form.trainingDays),
                    validationDays: Number(form.validationDays),
                  }
                : undefined,
          },
        });
        setShown({ kind: "sweep", id });
      } else {
        await lab({ case: "startBacktest", value: run });
        setShown({ kind: "run", id });
      }
      await readRuns();
    });
    setBusy(false);
  };

  // A sweep's runs show as one row.
  const rows = useMemo(() => {
    const seen = new Set<string>();
    return runs.filter(run => {
      if (!run.sweepId) return true;
      if (seen.has(run.sweepId)) return false;
      seen.add(run.sweepId);
      return true;
    });
  }, [runs]);

  const field = (label: string, key: keyof RunForm, props: Record<string, string> = {}) => (
    <label>
      <span>{t(label)}</span>
      <input
        {...props}
        value={form[key] as string}
        onChange={event => change({ [key]: event.target.value } as Partial<RunForm>)}
      />
    </label>
  );
  const dateField = (label: string, key: "begin" | "end" | "holdoutDay") => (
    <label>
      <span>{t(label)}</span>
      <DateInput value={form[key]} onChange={value => change({ [key]: value })} />
    </label>
  );
  const validation = { none: "不验证", holdout: "留出", rolling: "滚动验证" } as const;
  const [folded, setFolded] = useConfigFold();
  return (
    <section className="lab-research" data-folded={folded || undefined}>
      <LabHeader context={context} modules={strategies} refresh={() => void readStrategies()} />
      <aside className="lab-modules">
        <h4>{t("策略")}</h4>
        <ul>
          {strategies?.strategies.map(item => {
            const [module, name] = item.id.split(".");
            return (
              <li key={item.id}>
                <button
                  aria-pressed={item.id === chosen}
                  onClick={() => {
                    setChosen(item.id);
                    source.open(`strategies/${module}.py`);
                    change({
                      params: Object.fromEntries(
                        item.parameters.map(p => [p.name, defaultText(p)]),
                      ),
                    });
                  }}
                >
                  <strong>{name}</strong>
                  <small>{module}</small>
                  {item.description && (
                    <span>{item.description.split("\n")[0].replace(/`/g, "")}</span>
                  )}
                </button>
              </li>
            );
          })}
        </ul>
        <NewModule
          folder="strategies"
          repository={repository}
          created={path => {
            source.open(path);
            void readStrategies();
          }}
        />
        <ModuleErrors folder="strategies" modules={strategies} open={source.open} />
      </aside>
      <LabCentre
        source={source}
        repository={repository}
        hasResult={!!shown}
        changed={() => void readStrategies()}
        deleted={() => {
          setChosen("");
          void readStrategies();
        }}
      >
        <>
          {shown?.kind === "run" && (
            <RunResult
              key={shown.id}
              id={shown.id}
              state={runState(runs.find(run => run.id === shown.id)?.state ?? 0)}
              openVersion={version =>
                context.navigate("workspace.coverage", {
                  params: { version },
                })
              }
            />
          )}
          {shown?.kind === "sweep" && sweep?.id === shown.id && (
            <SweepResult
              runs={sweep.runs}
              selected={sweep.selected}
              open={id => setShown({ kind: "run", id })}
              rolling={
                runs.some(run => run.id === `${shown.id}-rolling`)
                  ? () => setShown({ kind: "run", id: `${shown.id}-rolling` })
                  : undefined
              }
            />
          )}
          {shown?.kind === "compare" && compared.length === 2 && (
            <RunComparison ids={[compared[0], compared[1]]} />
          )}
          {!shown && (
            <p className="lab-placeholder">
              {t("选择左侧的策略、填写右侧的配置后运行；结果显示在这里。")}
            </p>
          )}
        </>
      </LabCentre>
      <aside className="lab-config">
        <h4>
          {!folded && t("运行配置")}
          <ConfigFold folded={folded} fold={setFolded} />
        </h4>
        {!folded && (
          <>
            {!strategy ? (
              <p className="subtle">{t("先选择一个策略。")}</p>
            ) : (
              <>
                <div className="lab-form">
                  <label className="wide">
                    <span>{t("品种")}</span>
                    <input
                      value={form.symbols}
                      placeholder="SHFE/rb, SHFE/au/2026-12"
                      onChange={event => change({ symbols: event.target.value })}
                    />
                  </label>
                  {dateField("开始日", "begin")}
                  {dateField("结束日", "end")}
                  <label>
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
                  <h5 className="wide">{t("参数（多个值用逗号分隔即为扫描）")}</h5>
                  {strategy.parameters.map(parameter => (
                    <label key={parameter.name}>
                      <span>
                        {parameter.name} <small>{parameterKind(parameter)}</small>
                      </span>
                      <input
                        value={form.params[parameter.name] ?? ""}
                        onChange={event =>
                          change({
                            params: { ...form.params, [parameter.name]: event.target.value },
                          })
                        }
                      />
                    </label>
                  ))}
                  <h5 className="wide">{t("资金与交易")}</h5>
                  {field("初始资金", "deposit")}
                  {field("滑点（跳）", "slippage")}
                  {field("单笔手数", "maxOrderLots")}
                  {field("总持仓手数", "maxGrossLots")}
                  <details className="wide lab-section">
                    <summary>
                      {t("更多限额与统计")}
                      <small>
                        {t("每年 {periods} 期", { periods: form.annualPeriods || "—" })}
                      </small>
                    </summary>
                    <div className="lab-form">
                      {field("在途委托数", "maxWorkingOrders")}
                      {field("并行进程数", "processes", { placeholder: t("处理器数减一") })}
                      {field("总权重上限", "maxGrossWeight")}
                      {field("净权重上限", "maxNetWeight")}
                      {field("每年期数", "annualPeriods")}
                      <label className="lab-check">
                        <input
                          type="checkbox"
                          checked={form.zeroRiskFree}
                          onChange={event => change({ zeroRiskFree: event.target.checked })}
                        />
                        {t("无风险利率为零")}
                      </label>
                    </div>
                  </details>
                  <h5 className="wide">{t("验证")}</h5>
                  <div className="wide lab-segmented" role="radiogroup">
                    {(["none", "holdout", "rolling"] as const).map(kind => (
                      <button
                        key={kind}
                        role="radio"
                        aria-checked={form.validation === kind}
                        onClick={() => change({ validation: kind })}
                      >
                        {t(validation[kind])}
                      </button>
                    ))}
                  </div>
                  {form.validation === "holdout" && dateField("留出日", "holdoutDay")}
                  {form.validation === "rolling" && (
                    <>
                      {field("训练交易日数", "trainingDays")}
                      {field("每轮验证交易日数", "validationDays")}
                    </>
                  )}
                </div>
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
                  <ActionError error={submitted.error} />
                </footer>
              </>
            )}
          </>
        )}
      </aside>
      <footer className="lab-runs">
        <header>
          <h4>{t("运行记录")}</h4>
          <button
            className="lab-link"
            disabled={compared.length !== 2}
            onClick={() => setShown({ kind: "compare" })}
          >
            {t("比较所选两次")}
          </button>
        </header>
        <ActionError error={cancelled.error} />
        <table className="lab-table">
          <thead>
            <tr>
              <th />
              <th>{t("名称")}</th>
              <th>{t("策略")}</th>
              <th>{t("状态")}</th>
              <th className="numeric">{t("总收益")}</th>
              <th className="numeric">{t("夏普比率")}</th>
              <th className="numeric">{t("最大回撤")}</th>
              <th>{t("开始时间")}</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {rows.map(run => {
              const group = run.sweepId;
              const members = group ? runs.filter(item => item.sweepId === group) : [];
              const current =
                (shown?.kind === "run" && shown.id === run.id && !group) ||
                (shown?.kind === "sweep" && shown.id === group);
              return (
                <tr key={group || run.id} className={current ? "current" : undefined}>
                  <td>
                    {!group && (
                      <input
                        type="checkbox"
                        aria-label={t("比较 {id}", { id: run.id })}
                        checked={compared.includes(run.id)}
                        disabled={runState(run.state) !== "succeeded"}
                        onChange={event =>
                          setCompared(previous =>
                            event.target.checked
                              ? [...previous.slice(-1), run.id]
                              : previous.filter(item => item !== run.id),
                          )
                        }
                      />
                    )}
                  </td>
                  <td>
                    <button
                      className="lab-link strong"
                      onClick={() =>
                        setShown(group ? { kind: "sweep", id: group } : { kind: "run", id: run.id })
                      }
                    >
                      {group
                        ? t("扫描 {id}（{count} 组）", { id: group, count: members.length })
                        : run.id}
                    </button>
                  </td>
                  <td className="subtle">{run.strategy}</td>
                  <td>
                    {group ? (
                      t("已完成 {done} / {count}", {
                        done: members.filter(item => runState(item.state) === "succeeded").length,
                        count: members.length,
                      })
                    ) : (
                      <StatusBadge tone={runTones[runState(run.state)] ?? "neutral"}>
                        {t(states[runState(run.state)] ?? runState(run.state))}
                      </StatusBadge>
                    )}
                  </td>
                  <td className={`numeric ${signed(run.metrics.total_return)}`}>
                    {group ? "" : metric(run.metrics.total_return, true)}
                  </td>
                  <td className="numeric">{group ? "" : metric(run.metrics.sharpe, false)}</td>
                  <td className="numeric">{group ? "" : metric(run.metrics.max_drawdown, true)}</td>
                  <td className="subtle numeric-left">{started(run.startedMs)}</td>
                  <td>
                    {((runState(run.state) === "running" && !group) ||
                      (group && members.some(item => runState(item.state) === "running"))) && (
                      <button
                        className="lab-link"
                        onClick={() =>
                          void cancelled.attempt(async () => {
                            await (group
                              ? lab({ case: "cancelSweep", value: { id: group } })
                              : lab({ case: "cancelRun", value: { id: run.id } }));
                            await readRuns();
                          })
                        }
                      >
                        {t("取消")}
                      </button>
                    )}
                    {runState(run.state) !== "running" &&
                      !(group && members.some(item => runState(item.state) === "running")) && (
                        <DeleteRecord
                          name={group || run.id}
                          remove={() =>
                            void cancelled.attempt(async () => {
                              // A run of a sweep takes the whole sweep.
                              await lab({ case: "deleteRun", value: { id: run.id } });
                              if (shown && "id" in shown && shown.id === (group || run.id))
                                setShown(null);
                              await readRuns();
                            })
                          }
                        />
                      )}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </footer>
    </section>
  );
}

// When a run started, as month-day and time of the viewer's clock.
function started(ms: bigint) {
  const time = new Date(Number(ms));
  const two = (value: number) => String(value).padStart(2, "0");
  return `${two(time.getMonth() + 1)}-${two(time.getDate())} ${two(time.getHours())}:${two(time.getMinutes())}`;
}
