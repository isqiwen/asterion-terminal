import { DateInput } from "../../src/ui/DateInput";
import { useEffect, useState } from "react";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { lab } from "../../src/bridge/lab";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { RunSummary } from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import {
  defaultForm,
  defaultText,
  identity,
  parameterKind,
  runRequest,
  type RunForm,
} from "./research-lab";
import {
  ConfigFold,
  LabCentre,
  LabStatus,
  ModuleErrors,
  ModuleList,
  NewModule,
  Section,
  useConfigFold,
  useModules,
  useSource,
} from "./LabParts";
import { metric, signed } from "./ResearchResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// The strategy view: the strategy modules, their code and a run's
// configuration. A started run opens its report in the runs view.
export function StrategyLab(context: TerminalContext) {
  const repository = context.snapshot?.research_directory ?? "";
  const { modules: strategies, read: readStrategies } = useModules(repository);
  // Kept while a run's report is on show in the runs view.
  const [chosen, setChosen] = useWorkspaceDraft(`strategy:${repository}`, "");
  const [form, setForm] = useWorkspaceDraft<RunForm>("run-form", defaultForm);
  const source = useSource(repository);
  // The latest results beside each strategy and under the run button.
  const [runs, setRuns] = useState<RunSummary[]>([]);
  useEffect(() => {
    let active = true;
    void lab({ case: "listRuns", value: {} })
      .then(reply => active && setRuns(reply.runs))
      .catch(failure => active && notify(failure));
    return () => {
      active = false;
    };
  }, []);
  const [saved, setSaved] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const submitted = useActionError();

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
        context.navigate("workspace.lab-runs", { params: { sweep: id } });
      } else {
        await lab({ case: "startBacktest", value: run });
        context.navigate("workspace.lab-runs", { params: { run: id } });
      }
    });
    setBusy(false);
  };

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
      <aside className="lab-modules">
        <h4>{t("策略")}</h4>
        <ModuleList
          folder="strategies"
          chosen={chosen}
          items={(strategies?.strategies ?? []).map(item => {
            const [module, name] = item.id.split(".");
            // Runs list newest first.
            const latest = runs.find(run => run.strategy === item.id && !run.sweepId);
            const value = latest?.metrics.total_return;
            return {
              id: item.id,
              name,
              module,
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
            source.open(`strategies/${item.module}.py`);
            change({
              params: Object.fromEntries(entry.parameters.map(p => [p.name, defaultText(p)])),
            });
          }}
        />
        <NewModule
          folder="strategies"
          repository={repository}
          created={path => {
            source.open(path);
            void readStrategies();
          }}
        />
        <ModuleErrors folder="strategies" modules={strategies} open={source.open} />
        <LabStatus context={context} modules={strategies} refresh={() => void readStrategies()} />
      </aside>
      <LabCentre
        source={source}
        repository={repository}
        changed={() => void readStrategies()}
        deleted={() => {
          setChosen("");
          void readStrategies();
        }}
      >
        <>
          <p className="lab-placeholder">{t("选择策略")}</p>
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
              <p className="subtle">{t("选择策略")}</p>
            ) : (
              <>
                <Section
                  title={t("品种与区间")}
                  summary={`${form.symbols || "—"} · ${form.begin || "…"} – ${form.end || "…"} · ${
                    form.minutes === "0" ? t("日线") : t("{count} 分钟", { count: form.minutes })
                  }`}
                  open
                >
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
                </Section>
                <Section
                  title={t("参数")}
                  summary={strategy.parameters
                    .map(parameter => `${parameter.name}=${form.params[parameter.name] ?? ""}`)
                    .join(" ")}
                  open
                >
                  <p className="wide subtle">{t("多个值用逗号分隔即为扫描")}</p>
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
                </Section>
                <Section
                  title={t("资金与风控")}
                  summary={t("资金 {deposit} · 滑点 {slippage} 跳 · 单笔 {order} / 总 {gross} 手", {
                    deposit: form.deposit,
                    slippage: form.slippage,
                    order: form.maxOrderLots,
                    gross: form.maxGrossLots,
                  })}
                >
                  {field("初始资金", "deposit")}
                  {field("滑点（跳）", "slippage")}
                  {field("单笔手数", "maxOrderLots")}
                  {field("单合约持仓手数", "maxContractLots")}
                  {field("账户总持仓手数", "maxGrossLots")}
                  {field("在途委托数", "maxWorkingOrders")}
                  {field("总权重上限", "maxGrossWeight")}
                  {field("净权重上限", "maxNetWeight")}
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
                <Section title={t("扫描与验证")} summary={t(validation[form.validation])}>
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
                  {field("并行进程数", "processes", { placeholder: t("处理器数减一") })}
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
                  <ActionError error={submitted.error} />
                  <RecentRuns
                    runs={runs.filter(run => run.strategy === strategy.id && !run.sweepId)}
                    open={id => context.navigate("workspace.lab-runs", { params: { run: id } })}
                  />
                </footer>
              </>
            )}
          </>
        )}
      </aside>
    </section>
  );
}

// The strategy's last runs, each opening its report.
function RecentRuns({ runs, open }: { runs: RunSummary[]; open: (id: string) => void }) {
  if (!runs.length) return null;
  return (
    <section className="lab-recent" aria-label={t("最近运行")}>
      <h5>{t("最近运行")}</h5>
      {runs.slice(0, 3).map(run => (
        <button key={run.id} className="lab-link" onClick={() => open(run.id)}>
          <span>{run.label || run.id}</span>
          <span className={signed(run.metrics.total_return)}>
            {metric(run.metrics.total_return, true)}
          </span>
        </button>
      ))}
    </section>
  );
}
