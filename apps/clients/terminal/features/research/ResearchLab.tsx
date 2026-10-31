import { useCallback, useEffect, useMemo, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  lab,
  translate,
  type DisplayError,
  type LabRunSummary,
  type LabStrategies,
  type LabSummary,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import {
  costNames,
  defaultForm,
  defaultText,
  productsOf,
  runRequest,
  type RunForm,
} from "./research-lab";
import { RunComparison, RunResult, SweepResult, metric, signed, states } from "./ResearchResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// Runs and sweeps are named by strategy and the time they were asked for.
function identity(strategy: string) {
  const stamp = new Date().toISOString().replace(/[-:T]/g, "").slice(0, 14);
  return `${strategy.split(".")[0].replace(/[^A-Za-z0-9_-]/g, "-")}-${stamp}`;
}

type Shown =
  { kind: "run"; id: string } | { kind: "sweep"; id: string } | { kind: "compare" } | null;

// The research page: the user's strategies, a run's configuration, its result
// and every run so far, all on the lab service.
export function ResearchLab(context: TerminalContext) {
  const setting = context.snapshot?.strategy_repository;
  const repository = setting && "path" in setting ? setting.path : "";
  const [strategies, setStrategies] = useState<LabStrategies | null>(null);
  const [summary, setSummary] = useState<LabSummary | null>(null);
  const [chosen, setChosen] = useState("");
  const [form, setForm] = useState<RunForm>(defaultForm);
  const [runs, setRuns] = useState<LabRunSummary[]>([]);
  const [sweep, setSweep] = useState<{
    id: string;
    runs: LabRunSummary[];
    selected: string;
  } | null>(null);
  const [shown, setShown] = useState<Shown>(null);
  const [compared, setCompared] = useState<string[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const [busy, setBusy] = useState(false);

  const readStrategies = useCallback(async () => {
    if (!repository) return;
    setError("");
    try {
      setStrategies(await lab({ case: "strategies", value: { repository } }, "strategies"));
    } catch (failure) {
      setError(asDisplayError(failure));
    }
  }, [repository]);
  const readRuns = useCallback(async () => {
    try {
      setRuns((await lab({ case: "runs", value: {} }, "runs")).runs);
    } catch (failure) {
      setError(asDisplayError(failure));
    }
  }, []);
  useEffect(() => {
    void readStrategies();
    void readRuns();
    void lab({ case: "summary", value: { provider: "tushare" } }, "summary")
      .then(setSummary)
      .catch(failure => setError(asDisplayError(failure)));
  }, [readStrategies, readRuns]);
  const running = runs.some(run => run.state === "running");
  useEffect(() => {
    if (!running) return;
    const timer = setInterval(() => void readRuns(), 2000);
    return () => clearInterval(timer);
  }, [running, readRuns]);
  useEffect(() => {
    if (shown?.kind !== "sweep") return;
    let active = true;
    void lab({ case: "sweepRuns", value: { id: shown.id } }, "sweepRuns")
      .then(reply => {
        if (!active) return;
        setSweep({ id: reply.id, runs: [...reply.runs], selected: reply.selected });
      })
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [shown, runs]);

  const strategy = strategies?.strategies.find(item => item.id === chosen);
  const products = productsOf(form.symbols);
  const change = (patch: Partial<RunForm>) => setForm(previous => ({ ...previous, ...patch }));
  const submit = async () => {
    if (!strategy) return;
    setBusy(true);
    setError("");
    try {
      const id = identity(strategy.id);
      const { run, swept } = runRequest(form, repository, strategy.id, strategy.parameters, id);
      if (form.validation === "rolling" && !swept.length)
        throw new Error(t("滚动验证需要至少一个参数填多个值"));
      if (swept.length || form.validation === "rolling") {
        await lab(
          {
            case: "sweep",
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
          },
          "accepted",
        );
        setShown({ kind: "sweep", id });
      } else {
        await lab({ case: "backtest", value: run }, "accepted");
        setShown({ kind: "run", id });
      }
      await readRuns();
    } catch (failure) {
      setError(asDisplayError(failure));
    } finally {
      setBusy(false);
    }
  };

  // A sweep's runs show as one row.
  const rows = useMemo(() => {
    const seen = new Set<string>();
    return runs.filter(run => {
      if (!run.sweep) return true;
      if (seen.has(run.sweep)) return false;
      seen.add(run.sweep);
      return true;
    });
  }, [runs]);

  if (!repository)
    return (
      <section className="lab-research lab-empty">
        <h3>{t("研究")}</h3>
        <p className="subtle">
          {t(
            "先在 设置 → 研究 里选择你的策略仓库：一个自己的 Git 仓库，策略放在其中的 strategies/ 目录下。",
          )}
        </p>
        <button className="primary" onClick={() => context.openSettings("research")}>
          {t("选择策略仓库")}
        </button>
      </section>
    );
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
  const validation = { none: "不验证", holdout: "留出", rolling: "滚动验证" } as const;
  return (
    <section className="lab-research">
      <header className="lab-header">
        <h3>{t("研究")}</h3>
        <span className="lab-repository" title={repository}>
          {repository.split("/").filter(Boolean).pop()}
        </span>
        {strategies && <code className="lab-chip">{strategies.commit.slice(0, 8)}</code>}
        {strategies?.changes && <span className="lab-chip warn">{t("有未提交改动")}</span>}
        <span className="lab-spacer" />
        <button className="lab-link" onClick={() => void readStrategies()}>
          {t("刷新")}
        </button>
        <button className="lab-link" onClick={() => context.openSettings("research")}>
          {t("更换")}
        </button>
      </header>
      <ErrorNotice error={error} />
      <aside className="lab-strategies">
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
        {strategies?.errors.map(item => (
          <details key={item.module} className="lab-module-error">
            <summary>
              {item.module}.py · {t("导入失败")}
            </summary>
            <pre>{item.message}</pre>
          </details>
        ))}
        <h4>{t("数据")}</h4>
        <p className="subtle">
          Tushare · <code>{summary?.datasetVersion.slice(0, 8) || t("无数据")}</code>
        </p>
        <button className="lab-link" onClick={() => context.navigate("workspace.coverage")}>
          {t("查看数据覆盖")}
        </button>
      </aside>
      <main className="lab-center">
        {shown?.kind === "run" && <RunResult key={shown.id} id={shown.id} />}
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
            {t("选择左侧的策略、填写右侧的运行配置后运行；结果显示在这里。")}
          </p>
        )}
      </main>
      <aside className="lab-config">
        <h4>{t("运行配置")}</h4>
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
              {field("开始日", "begin", { type: "date" })}
              {field("结束日", "end", { type: "date" })}
              <h5 className="wide">{t("参数（多个值用逗号分隔即为扫描）")}</h5>
              {strategy.parameters.map(parameter => (
                <label key={parameter.name}>
                  <span>
                    {parameter.name} <small>{parameter.kind}</small>
                  </span>
                  <input
                    value={form.params[parameter.name] ?? ""}
                    onChange={event =>
                      change({ params: { ...form.params, [parameter.name]: event.target.value } })
                    }
                  />
                </label>
              ))}
              <h5 className="wide">{t("资金与交易")}</h5>
              {field("初始资金", "deposit")}
              {field("滑点（跳）", "slippage")}
              {field("单笔手数", "maxOrderLots")}
              {field("总持仓手数", "maxGrossLots")}
              {products.map(product => (
                <details key={product} className="wide lab-section">
                  <summary>
                    {t("{product} 的费用", { product })}
                    <small>
                      {(form.costs[product] ?? []).filter(Boolean).length
                        ? (form.costs[product] ?? []).join(" · ")
                        : t("未填写")}
                    </small>
                  </summary>
                  <div className="lab-form">
                    {costNames.map((name, index) => (
                      <label key={name}>
                        <span>{t(name)}</span>
                        <input
                          value={form.costs[product]?.[index] ?? ""}
                          onChange={event => {
                            const values = [...(form.costs[product] ?? costNames.map(() => ""))];
                            values[index] = event.target.value;
                            change({ costs: { ...form.costs, [product]: values } });
                          }}
                        />
                      </label>
                    ))}
                  </div>
                </details>
              ))}
              <details className="wide lab-section">
                <summary>
                  {t("更多限额与统计")}
                  <small>{t("每年 {periods} 期", { periods: form.annualPeriods || "—" })}</small>
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
              {form.validation === "holdout" && field("留出日", "holdoutDay", { type: "date" })}
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
            </footer>
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
              const group = run.sweep;
              const members = group ? runs.filter(item => item.sweep === group) : [];
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
                        disabled={run.state !== "succeeded"}
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
                  <td className={`lab-state ${group ? "" : run.state}`}>
                    {group
                      ? t("已完成 {done} / {count}", {
                          done: members.filter(item => item.state === "succeeded").length,
                          count: members.length,
                        })
                      : t(states[run.state] ?? run.state)}
                  </td>
                  <td className={`numeric ${signed(run.metrics.total_return)}`}>
                    {group ? "" : metric(run.metrics.total_return, true)}
                  </td>
                  <td className="numeric">{group ? "" : metric(run.metrics.sharpe, false)}</td>
                  <td className="numeric">{group ? "" : metric(run.metrics.max_drawdown, true)}</td>
                  <td className="subtle numeric-left">{started(run.startedNs)}</td>
                  <td>
                    {((run.state === "running" && !group) ||
                      (group && members.some(item => item.state === "running"))) && (
                      <button
                        className="lab-link"
                        onClick={() =>
                          void (
                            group
                              ? lab({ case: "cancelSweep", value: { id: group } }, "accepted")
                              : lab({ case: "cancelRun", value: { id: run.id } }, "accepted")
                          )
                            .then(readRuns)
                            .catch(failure => setError(asDisplayError(failure)))
                        }
                      >
                        {t("取消")}
                      </button>
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
function started(ns: bigint) {
  const time = new Date(Number(ns / 1_000_000n));
  const two = (value: number) => String(value).padStart(2, "0");
  return `${two(time.getMonth() + 1)}-${two(time.getDate())} ${two(time.getHours())}:${two(time.getMinutes())}`;
}
