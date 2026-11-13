import { DateInput } from "../../src/ui/DateInput";
import { useCallback, useEffect, useState } from "react";
import { lab } from "../../src/bridge/lab";
import type { FactorResult } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { notify } from "../../src/host/notifications";
import { type DisplayError, ErrorNotice, asDisplayError } from "../../src/i18n/errors";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, getLocale, translate, listSeparator } from "../../src/i18n";
import {
  defaultFactorForm,
  defaultText,
  factorRequest,
  identity,
  parameterKind,
  type FactorForm,
} from "./research-lab";
import { FactorResultView } from "./FactorResultView";
import {
  ConfigFold,
  DeleteRecord,
  LabCentre,
  LabHeader,
  ModuleErrors,
  ModuleList,
  type ModuleItem,
  NewModule,
  Section,
  useConfigFold,
  useModules,
  useSource,
} from "./LabParts";
import { metric, signed } from "./ResearchResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// The factor view: the factor modules, their code, an evaluation's settings and
// the chosen factor's kept evaluations, whose reports open in place of the code.
export function FactorLab(context: TerminalContext) {
  const repository = context.snapshot?.research_directory ?? "";
  const { modules, read } = useModules(repository);
  const [chosen, setChosen] = useWorkspaceDraft(`factor:${repository}`, "");
  const [report, setReport] = useWorkspaceDraft("factor-report", "");
  const [form, setForm] = useWorkspaceDraft<FactorForm>("factor-form", defaultFactorForm);
  const [busy, setBusy] = useState(false);
  const { error, attempt } = useActionError();
  const source = useSource(repository);
  const [saved, setSaved] = useState<string[]>([]);

  const factor = modules?.factors.find(item => item.id === chosen);
  const [library, setLibrary] = useWorkspaceDraft("factor-library", false);
  // Kept evaluations, newest first: each factor's latest beside it in the list
  // and in the library, and the chosen factor's under its settings.
  const [kept, setKept] = useState<FactorResult[]>([]);
  const readKept = useCallback(
    async () => setKept((await lab({ case: "listFactorResults", value: {} })).results),
    [],
  );
  useEffect(() => void readKept().catch(notify), [readKept]);
  const latest = (id: string) => kept.find(item => item.evaluation?.factor === id);
  const evaluations = kept.filter(item => item.evaluation?.factor === chosen);
  const items: ModuleItem[] = (modules?.factors ?? []).map(item => {
    const [module, name] = item.id.split(".");
    const ic = latest(item.id)?.horizons[0]?.mean;
    return {
      id: item.id,
      name,
      module,
      description: item.description.replace(/`/g, ""),
      inputs: item.inputs,
      figure:
        ic === undefined
          ? undefined
          : { text: metric(ic, false), className: signed(ic), title: t("最近一次 IC 均值") },
    };
  });
  const choose = (id: string) => {
    const entry = modules!.factors.find(item => item.id === id)!;
    setChosen(id);
    setReport("");
    source.open(`factors/${id.split(".")[0]}.py`);
    change({ params: Object.fromEntries(entry.parameters.map(p => [p.name, defaultText(p)])) });
  };
  const change = (patch: Partial<FactorForm>) => setForm(previous => ({ ...previous, ...patch }));
  const evaluate = async () => {
    if (!factor) return;
    setBusy(true);
    setSaved([]);
    await attempt(async () => {
      const written = await source.save();
      setSaved(written);
      if (written.length) await read();
      const id = identity(factor.id);
      await lab({
        case: "evaluateFactor",
        value: { ...factorRequest(form, repository, factor), id },
      });
      setReport(id);
      await readKept();
    });
    setBusy(false);
  };
  const remove = (id: string) =>
    void attempt(async () => {
      await lab({ case: "deleteFactorResult", value: { id } });
      setReport("");
      await readKept();
    });
  const field = (label: string, key: "begin" | "end" | "horizons" | "groups", props = {}) => (
    <label>
      <span>{t(label)}</span>
      <input
        {...props}
        value={form[key]}
        onChange={event => change({ [key]: event.target.value })}
      />
    </label>
  );

  const [folded, setFolded] = useConfigFold();
  return (
    <section className="lab-research lab-factors" data-folded={folded || undefined}>
      <LabHeader context={context} modules={modules} refresh={() => void read()} />
      <aside className="lab-modules">
        <h4>
          {t("因子")}
          <button className="lab-link" aria-pressed={library} onClick={() => setLibrary(!library)}>
            {t("因子库")}
          </button>
        </h4>
        <ModuleList
          folder="factors"
          chosen={chosen}
          items={items}
          choose={item => {
            choose(item.id);
            setLibrary(false);
          }}
        />
        {modules && !modules.factors.length && <p className="subtle">{t("还没有因子")}</p>}
        <NewModule
          folder="factors"
          repository={repository}
          created={path => {
            source.open(path);
            void read();
          }}
        />
        <ModuleErrors folder="factors" modules={modules} open={source.open} />
      </aside>
      {library ? (
        <FactorLibrary
          items={items}
          latest={latest}
          open={id => {
            choose(id);
            setLibrary(false);
          }}
        />
      ) : report ? (
        <main className="lab-center lab-runs-view">
          <header className="lab-report-header">
            <button className="lab-link" onClick={() => setReport("")}>
              {t("← 源码")}
            </button>
            <h3>{report}</h3>
            <span className="lab-spacer" />
            <DeleteRecord name={report} remove={() => remove(report)} />
          </header>
          <Report key={report} id={report} />
        </main>
      ) : (
        <LabCentre
          source={source}
          repository={repository}
          changed={() => void read()}
          deleted={() => {
            setChosen("");
            void read();
          }}
        >
          <p className="lab-placeholder">{t("选择因子")}</p>
        </LabCentre>
      )}
      <aside className="lab-config">
        <h4>
          {!folded && t("评价")}
          <ConfigFold folded={folded} fold={setFolded} />
        </h4>
        {!folded && (
          <>
            {!factor ? (
              <p className="subtle">{t("选择因子")}</p>
            ) : (
              <>
                <Section
                  title={t("品种与区间")}
                  summary={`${form.symbols || "—"} · ${form.begin || "…"} – ${form.end || "…"}`}
                  open
                >
                  <label className="wide">
                    <span>{t("品种")}</span>
                    <input
                      value={form.symbols}
                      placeholder={t("SHFE（整个交易所）或 SHFE/rb, SHFE/au")}
                      onChange={event => change({ symbols: event.target.value })}
                    />
                  </label>
                  <label>
                    <span>{t("开始日")}</span>
                    <DateInput value={form.begin} onChange={begin => change({ begin })} />
                  </label>
                  <label>
                    <span>{t("结束日")}</span>
                    <DateInput value={form.end} onChange={end => change({ end })} />
                  </label>
                </Section>
                {factor.parameters.length > 0 && (
                  <Section
                    title={t("参数")}
                    summary={factor.parameters
                      .map(parameter => `${parameter.name}=${form.params[parameter.name] ?? ""}`)
                      .join(" ")}
                    open
                  >
                    {factor.parameters.map(parameter => (
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
                )}
                <Section
                  title={t("评价设置")}
                  summary={t("持有期 {horizons} · {groups} 组", {
                    horizons: form.horizons,
                    groups: form.groups,
                  })}
                  open
                >
                  {field("持有期（K 线数）", "horizons")}
                  {field("分组数", "groups")}
                </Section>
                {evaluations.length > 0 && (
                  <Section title={t("评价记录")} summary={String(evaluations.length)} open>
                    <ul className="lab-evaluations" aria-label={t("评价记录")}>
                      {evaluations.map(item => {
                        const ic = item.horizons[0]?.mean;
                        return (
                          <li key={item.id}>
                            <button
                              aria-pressed={item.id === report}
                              title={item.id}
                              onClick={() => setReport(item.id)}
                            >
                              <span>
                                {Object.entries(item.evaluation?.params ?? {})
                                  .map(([name, value]) => `${name}=${String(value.value.value)}`)
                                  .join(" ") || item.id}
                              </span>
                              <small>
                                {item.evaluation?.firstDay} – {item.evaluation?.lastDay}
                              </small>
                              <span className={`numeric ${signed(ic)}`}>{metric(ic, false)}</span>
                            </button>
                          </li>
                        );
                      })}
                    </ul>
                  </Section>
                )}
                <footer className="lab-submit">
                  <button
                    className="primary"
                    disabled={busy || context.busy}
                    onClick={() => void evaluate()}
                  >
                    {busy ? t("正在评价…") : t("评价")}
                  </button>
                  {saved.length > 0 && (
                    <p className="subtle">
                      {t("已保存 {files}", { files: saved.join(listSeparator()) })}
                    </p>
                  )}
                  <ActionError error={error} />
                </footer>
              </>
            )}
          </>
        )}
      </aside>
    </section>
  );
}

type LibraryKey = "ic" | "ir" | "t" | "evaluated";

// Every factor with its latest evaluation, sortable by its figures.
function FactorLibrary({
  items,
  latest,
  open,
}: {
  items: readonly ModuleItem[];
  latest: (id: string) => FactorResult | undefined;
  open: (id: string) => void;
}) {
  const [sort, setSort] = useState<{ key: LibraryKey; descending: boolean }>({
    key: "ic",
    descending: true,
  });
  const value = (id: string) => {
    const result = latest(id);
    const first = result?.horizons[0];
    return {
      ic: first?.mean,
      ir: first?.ir,
      t: first?.t,
      evaluated: result ? Number(result.evaluatedMs) : undefined,
    }[sort.key];
  };
  const rows = [...items].sort((a, b) => {
    const [x, y] = [value(a.id), value(b.id)];
    if (x === undefined || y === undefined) return x === undefined ? (y === undefined ? 0 : 1) : -1;
    return sort.descending ? y - x : x - y;
  });
  const heading = (key: LibraryKey, label: string, title?: string) => (
    <th
      className="numeric"
      title={title}
      aria-sort={sort.key === key ? (sort.descending ? "descending" : "ascending") : undefined}
    >
      <button
        className="lab-sort"
        onClick={() =>
          setSort(previous => ({
            key,
            descending: previous.key === key ? !previous.descending : true,
          }))
        }
      >
        {label}
        {sort.key === key ? (sort.descending ? " ↓" : " ↑") : ""}
      </button>
    </th>
  );
  return (
    <main className="lab-center">
      <div className="lab-scroll">
        <table className="lab-table lab-records" aria-label={t("因子库")}>
          <thead>
            <tr>
              <th>{t("因子")}</th>
              <th>{t("模块")}</th>
              <th>{t("输入数据")}</th>
              {heading("ic", t("IC 均值"), t("最近一次评价的第一个持有期"))}
              {heading("ir", "ICIR")}
              {heading("t", t("t 值"))}
              {heading("evaluated", t("评价时间"))}
            </tr>
          </thead>
          <tbody>
            {rows.map(item => {
              const result = latest(item.id);
              const first = result?.horizons[0];
              return (
                <tr key={item.id} title={item.description}>
                  <td>
                    <button className="lab-link strong" onClick={() => open(item.id)}>
                      {item.name}
                    </button>
                  </td>
                  <td className="subtle">{item.module}</td>
                  <td className="subtle">{item.inputs?.join(listSeparator())}</td>
                  <td className={`numeric ${signed(first?.mean)}`}>{metric(first?.mean, false)}</td>
                  <td className="numeric">{metric(first?.ir, false)}</td>
                  <td className="numeric">{metric(first?.t, false)}</td>
                  <td className="subtle numeric">
                    {result
                      ? new Date(Number(result.evaluatedMs)).toLocaleString(getLocale(), {
                          hour12: false,
                        })
                      : "—"}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </main>
  );
}

function Report({ id }: { id: string }) {
  const [result, setResult] = useState<FactorResult | null>(null);
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void lab({ case: "getFactorResult", value: { id } })
      .then(reply => active && setResult(reply))
      .catch(failure => active && setError(asDisplayError(failure)));
    return () => {
      active = false;
    };
  }, [id]);
  if (error) return <ErrorNotice error={error} />;
  if (!result) return <p className="subtle">{t("正在读取结果…")}</p>;
  return <FactorResultView result={result} />;
}
