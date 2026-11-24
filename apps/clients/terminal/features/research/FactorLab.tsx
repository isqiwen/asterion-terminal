import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { lab } from "../../src/bridge/lab";
import type { FactorResult } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { notify, useErrorNotification } from "../../src/host/notifications";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import { StatusBadge } from "../../src/ui/StatusBadge";
import {
  defaultText,
  factorRequest,
  factorScopeOf,
  factorTurnover,
  identity,
  onFactorScope,
  recordName,
  recordTime,
  savedFactorForm,
  symbolsOf,
  type FactorForm,
} from "./research-lab";
import { FactorResultView } from "./FactorResultView";
import { FactorScopeBar, useFactorConditions } from "./FactorScope";
import {
  ConfigFold,
  datePlaceholders,
  DeleteRecord,
  filterModuleItems,
  LabCentre,
  ModuleErrors,
  type ModuleItem,
  NewModule,
  Section,
  useConfigFold,
  useLibrarySort,
  useModuleMenu,
  useModules,
  useSource,
  useStoredCatalog,
} from "./LabParts";
import { metric, shortTime, signed } from "./ResearchResult";

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
  const [tab, setTab] = useWorkspaceDraft<"result" | "code">("factor-tab", "code");
  const [search, setSearch] = useWorkspaceDraft("modules-search:factors", "");
  const [busy, setBusy] = useState(false);
  const [batchProgress, setBatchProgress] = useState<{ done: number; total: number } | null>(null);
  const { error, attempt } = useActionError();
  const source = useSource(repository, "factors");
  const [saved, setSaved] = useState<string[]>([]);
  const [selected, setSelected] = useState<string[]>([]);

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
  const readKept = useCallback(
    async () => setKept((await lab({ case: "listFactorResults", value: {} })).results),
    [],
  );
  useEffect(() => void readKept().catch(notify), [readKept]);
  const catalog = useStoredCatalog(context, false, true);
  const enteredSymbols = symbolsOf(form.symbols);
  const [scopeBegin, scopeEnd] = datePlaceholders(catalog, enteredSymbols, false);
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
    setView("library");
  };
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
      if (openPage.current === factor.id) {
        setReport(id);
        setTab("result");
      }
      await readKept();
    });
    setBusy(false);
  };
  const activeSelected = selected.filter(id => modules?.factors.some(item => item.id === id));
  const evaluateBatch = async () => {
    const targets = activeSelected
      .map(id => modules?.factors.find(item => item.id === id))
      .filter((entry): entry is NonNullable<typeof entry> => entry !== undefined);
    if (!targets.length) return;
    setBusy(true);
    setSaved([]);
    setBatchProgress({ done: 0, total: targets.length });
    await attempt(async () => {
      const requests = targets.map((entry, index) => ({
        id: identity(entry.id, String(index + 1)),
        request: factorRequest(form, repository, entry, { useDefaults: true, extraFactors: [] }),
      }));
      const written = await source.save();
      setSaved(written);
      if (written.length) await read();
      try {
        for (let i = 0; i < requests.length; i++) {
          const item = requests[i]!;
          await lab({ case: "evaluateFactor", value: { ...item.request, id: item.id } });
          setBatchProgress({ done: i + 1, total: requests.length });
        }
      } finally {
        await readKept();
      }
    });
    setBatchProgress(null);
    setBusy(false);
  };
  const evaluateComposite = async () => {
    const targets = activeSelected
      .map(id => modules?.factors.find(item => item.id === id))
      .filter((entry): entry is NonNullable<typeof entry> => entry !== undefined);
    if (targets.length < 2) return;
    setBusy(true);
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
      await lab({ case: "evaluateFactor", value: { ...request, id } });
      if (!openPage.current) setReport(id);
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
  const savedNote = saved.length > 0 && (
    <p className="subtle">{t("已保存 {files}", { files: saved.join(listSeparator()) })}</p>
  );
  const sameName = (item: ModuleItem) => items.filter(other => other.name === item.name).length > 1;
  const reportName = shownResult ? resultName(shownResult) : recordName(report);
  const showsResult = !library && tab === "result" && Boolean(report);
  return (
    <section className="lab-research" data-folded={(!library && folded) || undefined}>
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
          {showsResult && <h2>{shownResult ? recordTime(shownResult.id) : reportName}</h2>}
          {showsResult && shownResult && inScope && !inScope(shownResult) && (
            <StatusBadge tone="neutral">{t("条件不同")}</StatusBadge>
          )}
          <span className="lab-spacer" />
          {showsResult && <DeleteRecord name={report} remove={() => remove(report)} />}
        </header>
      )}
      <FactorScopeBar
        context={context}
        form={form}
        begin={scopeBegin}
        end={scopeEnd}
        conditions={conditions}
        catalog={catalog}
      />
      {menu.overlay}
      {library && report ? (
        <div className="lab-center lab-runs-view">
          <header className="lab-report-header">
            <button type="button" className="lab-link" onClick={() => setReport("")}>
              {t("← 因子库")}
            </button>
            <h2>{reportName}</h2>
            <span className="lab-spacer" />
            <DeleteRecord name={report} remove={() => remove(report)} />
          </header>
          <Report key={report} id={report} />
        </div>
      ) : library ? (
        <FactorLibrary
          items={items}
          search={search}
          setSearch={setSearch}
          chosen={chosen}
          selected={activeSelected}
          setSelected={setSelected}
          latest={latest}
          onScope={inScope}
          composites={composites}
          busy={busy || Boolean(context.busy)}
          batchProgress={batchProgress}
          onBatchEvaluate={() => void evaluateBatch()}
          onCompositeEvaluate={() => void evaluateComposite()}
          open={choose}
          openComposite={setReport}
          contextTarget={menu.target}
          onMenu={menu.open}
          combination={combinationField("lab-inline")}
          newModule={
            <NewModule
              key={menu.copying?.id ?? "new"}
              folder="factors"
              repository={repository}
              copying={menu.copying}
              cancelCopy={menu.clearCopy}
              created={(path, id) => {
                openCode(path, id);
                void read();
              }}
            />
          }
          notices={
            <>
              <ActionError error={error} namespace="asterion.terminal.research" />
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
        <div className="lab-detail">
          {factor && (
            <nav className="tabs" aria-label={t("因子")}>
              {(
                [
                  ["result", "评价"],
                  ["code", "代码"],
                ] as const
              ).map(([key, label]) => (
                <button
                  key={key}
                  type="button"
                  aria-pressed={tab === key}
                  onClick={() => setTab(key)}
                >
                  {t(label)}
                </button>
              ))}
            </nav>
          )}
          {tab === "result" ? (
            report ? (
              <div className="lab-center lab-runs-view">
                <Report key={report} id={report} />
              </div>
            ) : (
              <p className="lab-placeholder">{t("还没有评价")}</p>
            )
          ) : (
            <LabCentre
              source={source}
              repository={repository}
              modules={modules}
              focus={chosen}
              changed={() => void read()}
              onDelete={path => menu.remove(path)}
            />
          )}
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
                <ExtraFactors
                  items={items.filter(item => item.id !== factor.id)}
                  value={form.extraFactors ?? []}
                  onChange={extraFactors => change({ extraFactors })}
                >
                  {combinationField("wide")}
                </ExtraFactors>
              )}
              {evaluations.length > 0 && (
                <Section title={t("评价记录")} open>
                  <ul className="lab-evaluations" aria-label={t("评价记录")}>
                    {evaluations.map(item => {
                      const ic = item.horizons[0]?.mean;
                      const label = isCompositeResult(item)
                        ? compositeName(item)
                        : Object.entries(item.evaluation?.params ?? {})
                            .map(([name, value]) => `${name}=${String(value.value.value)}`)
                            .join(" ") || recordName(item.id);
                      return (
                        <li key={item.id}>
                          <button
                            type="button"
                            aria-pressed={item.id === report}
                            title={item.id}
                            onClick={() => {
                              setReport(item.id);
                              setTab("result");
                            }}
                          >
                            <span>{label}</span>
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
                <button
                  type="button"
                  className="primary"
                  disabled={busy || context.busy}
                  onClick={() => void evaluate()}
                >
                  {busy ? t("正在评价…") : t("评价")}
                </button>
                {savedNote}
                <ActionError error={error} namespace="asterion.terminal.research" />
              </footer>
            </>
          )}
        </aside>
      )}
    </section>
  );
}

// The factors combined with the chosen one, found by search.
function ExtraFactors({
  items,
  value,
  onChange,
  children,
}: {
  items: readonly ModuleItem[];
  value: readonly string[];
  onChange: (next: string[]) => void;
  children: ReactNode;
}) {
  const [search, setSearch] = useState("");
  const count = items.filter(item => value.includes(item.id)).length;
  return (
    <Section title={t("合成因子（{count}）", { count })}>
      {children}
      <input
        type="search"
        className="wide"
        aria-label={t("搜索合成因子")}
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
      <td className={`numeric ${signed(value.ic)}`}>{result && metric(value.ic, false)}</td>
      <td className={`numeric ${signed(value.holdoutIc)}`}>
        {result && metric(value.holdoutIc, false)}
      </td>
      <td className="numeric">{result && metric(value.ir, false)}</td>
      <td className="numeric">{result && metric(value.t, false)}</td>
      <td className="numeric">{result && metric(value.turnover, true)}</td>
      <td className="subtle numeric">{result && shortTime(result.evaluatedMs)}</td>
    </>
  );
}

// Every factor beside its evaluation on the current settings. Results on other
// settings and factors without one are listed apart, so they never rank.
function FactorLibrary({
  items: all,
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
  onBatchEvaluate,
  onCompositeEvaluate,
  open,
  openComposite,
  contextTarget,
  onMenu,
  combination,
  newModule,
  notices,
}: {
  items: readonly ModuleItem[];
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
  onBatchEvaluate: () => void;
  onCompositeEvaluate: () => void;
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
        <button type="button" disabled={busy || selected.length < 2} onClick={onCompositeEvaluate}>
          {busy && !batchProgress
            ? t("正在评价…")
            : selected.length
              ? t("相关性与合成（{count}）", { count: selected.length })
              : t("相关性与合成")}
        </button>
        <button
          type="button"
          className="primary"
          disabled={busy || selected.length === 0}
          onClick={onBatchEvaluate}
        >
          {batchProgress
            ? t("正在评价 {done} / {total}…", batchProgress)
            : selected.length
              ? t("批量评价（{count}）", { count: selected.length })
              : t("批量评价")}
        </button>
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
                      })}
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
  useErrorNotification(error, "asterion.terminal.research");
  if (error) return null;
  if (!result) return <p className="subtle">{t("正在读取结果…")}</p>;
  return <FactorResultView result={result} />;
}
