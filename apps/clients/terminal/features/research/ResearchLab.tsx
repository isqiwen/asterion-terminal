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
import { RunComparison, RunResult, SweepResult, metric, states } from "./ResearchResult";

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
        <h2>{t("研究")}</h2>
        <p>
          {t(
            "先在 设置 → 研究 里选择你的策略仓库：一个自己的 Git 仓库，策略放在其中的 strategies/ 目录下。",
          )}
        </p>
        <button className="primary" onClick={() => context.openSettings("research")}>
          {t("选择策略仓库")}
        </button>
      </section>
    );
  return (
    <section className="lab-research">
      <header className="lab-header">
        <span>
          {t("策略仓库")} <code>{repository}</code>
        </span>
        {strategies && (
          <span>
            @<code>{strategies.commit.slice(0, 8)}</code>
            {strategies.changes && <strong className="lab-dirty"> ● {t("有未提交改动")}</strong>}
          </span>
        )}
        <button onClick={() => void readStrategies()}>{t("刷新")}</button>
        <button onClick={() => context.openSettings("research")}>{t("更换")}</button>
      </header>
      <ErrorNotice error={error} />
      <aside className="lab-strategies">
        <h3>{t("策略")}</h3>
        <ul>
          {strategies?.strategies.map(item => (
            <li key={item.id}>
              <button
                aria-pressed={item.id === chosen}
                title={item.description}
                onClick={() => {
                  setChosen(item.id);
                  change({
                    params: Object.fromEntries(item.parameters.map(p => [p.name, defaultText(p)])),
                  });
                }}
              >
                {item.id}
              </button>
            </li>
          ))}
        </ul>
        {strategies?.errors.map(item => (
          <details key={item.module} className="lab-module-error">
            <summary>
              ⚠ {item.module}.py {t("导入失败")}
            </summary>
            <pre>{item.message}</pre>
          </details>
        ))}
        <h3>{t("数据")}</h3>
        <p className="subtle">
          tushare · <code>{summary?.datasetVersion.slice(0, 8) || t("无数据")}</code>
        </p>
        <button className="link" onClick={() => context.navigate("workspace.coverage")}>
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
          />
        )}
        {shown?.kind === "sweep" && runs.some(run => run.id === `${shown.id}-rolling`) && (
          <button
            className="link"
            onClick={() => setShown({ kind: "run", id: `${shown.id}-rolling` })}
          >
            {t("查看滚动验证的账户")}
          </button>
        )}
        {shown?.kind === "compare" && compared.length === 2 && (
          <RunComparison ids={[compared[0], compared[1]]} />
        )}
        {!shown && (
          <p className="subtle">
            {t("选择左侧的策略、填写右侧的运行配置后运行；结果显示在这里。")}
          </p>
        )}
      </main>
      <aside className="lab-config">
        <h3>{t("运行配置")}</h3>
        {!strategy ? (
          <p className="subtle">{t("先选择一个策略。")}</p>
        ) : (
          <div className="lab-form">
            <label>
              {t("品种")}
              <input
                value={form.symbols}
                placeholder="SHFE/rb, SHFE/au/2026-12"
                onChange={event => change({ symbols: event.target.value })}
              />
            </label>
            <label>
              {t("开始日")}
              <input
                type="date"
                value={form.begin}
                onChange={event => change({ begin: event.target.value })}
              />
            </label>
            <label>
              {t("结束日")}
              <input
                type="date"
                value={form.end}
                onChange={event => change({ end: event.target.value })}
              />
            </label>
            <fieldset>
              <legend>{t("参数（多个值用逗号分隔即为扫描）")}</legend>
              {strategy.parameters.map(parameter => (
                <label key={parameter.name}>
                  {parameter.name} <small>{parameter.kind}</small>
                  <input
                    value={form.params[parameter.name] ?? ""}
                    onChange={event =>
                      change({ params: { ...form.params, [parameter.name]: event.target.value } })
                    }
                  />
                </label>
              ))}
            </fieldset>
            <label>
              {t("初始资金")}
              <input
                value={form.deposit}
                onChange={event => change({ deposit: event.target.value })}
              />
            </label>
            {products.map(product => (
              <fieldset key={product}>
                <legend>{t("{product} 的费用", { product })}</legend>
                {costNames.map((name, index) => (
                  <label key={name}>
                    {t(name)}
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
              </fieldset>
            ))}
            <label>
              {t("滑点（跳）")}
              <input
                value={form.slippage}
                onChange={event => change({ slippage: event.target.value })}
              />
            </label>
            <fieldset>
              <legend>{t("限额")}</legend>
              <label>
                {t("单笔手数")}
                <input
                  value={form.maxOrderLots}
                  onChange={event => change({ maxOrderLots: event.target.value })}
                />
              </label>
              <label>
                {t("总持仓手数")}
                <input
                  value={form.maxGrossLots}
                  onChange={event => change({ maxGrossLots: event.target.value })}
                />
              </label>
              <label>
                {t("在途委托数")}
                <input
                  value={form.maxWorkingOrders}
                  onChange={event => change({ maxWorkingOrders: event.target.value })}
                />
              </label>
              <label>
                {t("总权重上限")}
                <input
                  value={form.maxGrossWeight}
                  onChange={event => change({ maxGrossWeight: event.target.value })}
                />
              </label>
              <label>
                {t("净权重上限")}
                <input
                  value={form.maxNetWeight}
                  onChange={event => change({ maxNetWeight: event.target.value })}
                />
              </label>
            </fieldset>
            <fieldset>
              <legend>{t("统计")}</legend>
              <label>
                {t("每年期数")}
                <input
                  value={form.annualPeriods}
                  onChange={event => change({ annualPeriods: event.target.value })}
                />
              </label>
              <label className="lab-check">
                <input
                  type="checkbox"
                  checked={form.zeroRiskFree}
                  onChange={event => change({ zeroRiskFree: event.target.checked })}
                />
                {t("无风险利率为零")}
              </label>
            </fieldset>
            <fieldset>
              <legend>{t("验证")}</legend>
              {(["none", "holdout", "rolling"] as const).map(kind => (
                <label key={kind} className="lab-check">
                  <input
                    type="radio"
                    checked={form.validation === kind}
                    onChange={() => change({ validation: kind })}
                  />
                  {t({ none: "不验证", holdout: "留出", rolling: "滚动验证" }[kind])}
                </label>
              ))}
              {form.validation === "holdout" && (
                <label>
                  {t("留出日")}
                  <input
                    type="date"
                    value={form.holdoutDay}
                    onChange={event => change({ holdoutDay: event.target.value })}
                  />
                </label>
              )}
              {form.validation === "rolling" && (
                <>
                  <label>
                    {t("训练交易日数")}
                    <input
                      value={form.trainingDays}
                      onChange={event => change({ trainingDays: event.target.value })}
                    />
                  </label>
                  <label>
                    {t("每轮验证交易日数")}
                    <input
                      value={form.validationDays}
                      onChange={event => change({ validationDays: event.target.value })}
                    />
                  </label>
                </>
              )}
            </fieldset>
            <label>
              {t("并行进程数（空为处理器数减一）")}
              <input
                value={form.processes}
                onChange={event => change({ processes: event.target.value })}
              />
            </label>
            <button
              className="primary"
              disabled={busy || context.busy}
              onClick={() => void submit()}
            >
              {t("运行")}
            </button>
          </div>
        )}
      </aside>
      <footer className="lab-runs">
        <header>
          <h3>{t("运行记录")}</h3>
          <button disabled={compared.length !== 2} onClick={() => setShown({ kind: "compare" })}>
            {t("比较所选两次")}
          </button>
        </header>
        <div className="research-table">
          <table>
            <thead>
              <tr>
                <th>{t("比较")}</th>
                <th>{t("名称")}</th>
                <th>{t("策略")}</th>
                <th>{t("状态")}</th>
                <th>{t("总收益")}</th>
                <th>{t("夏普比率")}</th>
                <th>{t("最大回撤")}</th>
                <th>{t("开始时间")}</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {rows.map(run => {
                const group = run.sweep;
                const members = group ? runs.filter(item => item.sweep === group) : [];
                return (
                  <tr key={group || run.id}>
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
                      {group
                        ? t("扫描 {id}（{count} 组）", { id: group, count: members.length })
                        : run.id}
                    </td>
                    <td>{run.strategy}</td>
                    <td>
                      {group
                        ? t("已完成 {done} / {count}", {
                            done: members.filter(item => item.state === "succeeded").length,
                            count: members.length,
                          })
                        : t(states[run.state] ?? run.state)}
                    </td>
                    <td>{group ? "" : metric(run.metrics.total_return, true)}</td>
                    <td>{group ? "" : metric(run.metrics.sharpe, false)}</td>
                    <td>{group ? "" : metric(run.metrics.max_drawdown, true)}</td>
                    <td>{new Date(Number(run.startedNs / 1_000_000n)).toLocaleString()}</td>
                    <td>
                      <button
                        className="link"
                        onClick={() =>
                          setShown(
                            group ? { kind: "sweep", id: group } : { kind: "run", id: run.id },
                          )
                        }
                      >
                        {t("查看")}
                      </button>
                      {run.state === "running" && !group && (
                        <button
                          className="link"
                          onClick={() =>
                            void lab({ case: "cancelRun", value: { id: run.id } }, "accepted")
                              .then(readRuns)
                              .catch(failure => setError(asDisplayError(failure)))
                          }
                        >
                          {t("取消")}
                        </button>
                      )}
                      {group && members.some(item => item.state === "running") && (
                        <button
                          className="link"
                          onClick={() =>
                            void lab({ case: "cancelSweep", value: { id: group } }, "accepted")
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
        </div>
      </footer>
    </section>
  );
}
