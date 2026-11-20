import { DateInput } from "../../src/ui/DateInput";
import { useCallback, useEffect, useRef, useState } from "react";
import { lab } from "../../src/bridge/lab";
import type { FactorResult, StrategyEntry } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { notify, useErrorNotification } from "../../src/host/notifications";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import {
  defaultFactorForm,
  defaultText,
  factorRequest,
  factorTurnover,
  identity,
  recordName,
  symbolsOf,
  type FactorForm,
} from "./research-lab";
import { FactorResultView } from "./FactorResultView";
import {
  ConfigFold,
  datePlaceholders,
  DeleteRecord,
  filterModuleItems,
  LabCentre,
  ModuleErrors,
  ModuleList,
  type ModuleItem,
  NewModule,
  Section,
  SymbolField,
  useConfigFold,
  useModuleMenu,
  useModules,
  useSource,
  useStoredCatalog,
} from "./LabParts";
import { metric, signed } from "./ResearchResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const isCompositeResult = (item: FactorResult) =>
  item.factorNames.length >= 2 || (item.evaluation?.extraFactors.length ?? 0) > 0;

// The factor view: the factor modules, their code, an evaluation's settings and
// the chosen factor's kept evaluations, whose reports open in place of the code.
export function FactorLab(context: TerminalContext) {
  const repository = context.snapshot?.research_directory ?? "";
  const { modules, read, forget } = useModules(repository);
  const [chosen, setChosen] = useWorkspaceDraft(`factor:${repository}`, "");
  const [report, setReport] = useWorkspaceDraft("factor-report", "");
  const [form, setForm] = useWorkspaceDraft<FactorForm>("factor-form", defaultFactorForm);
  const [busy, setBusy] = useState(false);
  const [batchProgress, setBatchProgress] = useState<{ done: number; total: number } | null>(null);
  const { error, attempt } = useActionError();
  const source = useSource(repository, "factors");
  const [saved, setSaved] = useState<string[]>([]);
  const [selected, setSelected] = useState<string[]>([]);

  const factor = modules?.factors.find(item => item.id === chosen);
  const strategyUsers = useCallback(
    (id: string): readonly StrategyEntry[] =>
      (modules?.strategies ?? []).filter(item => item.factorModules.includes(id.split(".")[0])),
    [modules],
  );
  const users = chosen ? strategyUsers(chosen) : [];
  const [library, setLibrary] = useWorkspaceDraft("factor-library", false);
  const [search] = useWorkspaceDraft("modules-search:factors", "");
  // Kept evaluations, newest first: each factor's latest beside it in the list
  // and in the library, and the chosen factor's under its settings.
  const [kept, setKept] = useState<FactorResult[]>([]);
  const readKept = useCallback(
    async () => setKept((await lab({ case: "listFactorResults", value: {} })).results),
    [],
  );
  useEffect(() => void readKept().catch(notify), [readKept]);
  const latest = (id: string) =>
    kept.find(item => item.evaluation?.factor === id && !isCompositeResult(item));
  const composites = kept.filter(isCompositeResult);
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
    if (id !== chosen) {
      change({ params: Object.fromEntries(entry.parameters.map(p => [p.name, defaultText(p)])) });
    }
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

  useEffect(() => {
    if (!chosen || !modules || modules.factors.some(item => item.id === chosen)) return;
    const [stem] = chosen.split(".");
    const renamed = modules.factors.find(item => item.id.startsWith(`${stem}.`));
    if (renamed) setChosen(renamed.id);
  }, [chosen, modules, setChosen]);

  useEffect(() => {
    if (chosen || source.path || !modules?.factors.length) return;
    const first = [...modules.factors].sort((a, b) => {
      const [am = "", an = ""] = a.id.split(".");
      const [bm = "", bn = ""] = b.id.split(".");
      return an.localeCompare(bn) || am.localeCompare(bm);
    })[0]!;
    setChosen(first.id);
    source.open(`factors/${first.id.split(".")[0]}.py`);
    setForm(previous => ({
      ...previous,
      params: Object.fromEntries(
        first.parameters.map(p => [p.name, previous.params[p.name] ?? defaultText(p)]),
      ),
    }));
  }, [chosen, source, modules, setChosen, setForm]);

  const [folded, setFolded] = useConfigFold();
  const catalog = useStoredCatalog(context, false, true);
  const [beginPlaceholder, endPlaceholder] = datePlaceholders(
    catalog,
    symbolsOf(form.symbols),
    false,
  );
  const menu = useModuleMenu({
    repository,
    source,
    deleted: path => {
      forget(path);
      const stem = path.replace(/^factors\//, "").replace(/\.py$/, "");
      if (chosen.startsWith(`${stem}.`)) {
        setChosen("");
        setLibrary(false);
      }
      setSelected(previous => previous.filter(id => !id.startsWith(`${stem}.`)));
      void read();
    },
  });
  const shown = filterModuleItems(items, search);
  const sharedSettings = (
    <Section title={t("品种与区间")} open>
      <SymbolField
        context={context}
        value={form.symbols}
        allowExchanges
        catalog={catalog}
        onChange={symbols => change({ symbols })}
      />
      <label>
        <span>{t("开始日")}</span>
        <DateInput
          value={form.begin}
          placeholder={beginPlaceholder}
          onChange={begin => change({ begin })}
        />
      </label>
      <label>
        <span>{t("结束日")}</span>
        <DateInput
          value={form.end}
          placeholder={endPlaceholder}
          onChange={end => change({ end })}
        />
      </label>
    </Section>
  );
  const evaluationFields = (
    <>
      {field("持有期（K 线数）", "horizons")}
      {field("分组数", "groups")}
      <label>
        <span>{t("截面处理")}</span>
        <select
          value={form.standardize ?? ""}
          onChange={event =>
            change({ standardize: event.target.value as FactorForm["standardize"] })
          }
        >
          <option value="">{t("原始值")}</option>
          <option value="zscore">Z-Score</option>
          <option value="rank">{t("截面排名")}</option>
        </select>
      </label>
      <label>
        <span>{t("留出日")}</span>
        <DateInput value={form.holdoutDay ?? ""} onChange={holdoutDay => change({ holdoutDay })} />
      </label>
    </>
  );
  const combinationField = (
    <label className="wide">
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
  return (
    <section className="lab-research" data-folded={folded || undefined}>
      <aside className="lab-modules" aria-label={t("因子")}>
        <h2>{t("因子")}</h2>
        <button
          type="button"
          className="lab-module-item"
          aria-pressed={library}
          onClick={() => {
            if (!library) setReport("");
            setLibrary(!library);
          }}
        >
          <span>{t("因子库")}</span>
          {items.length > 0 && <small>{items.length}</small>}
        </button>
        <hr className="lab-divider" />
        <ModuleList
          folder="factors"
          chosen={library ? "" : chosen}
          contextTarget={menu.target}
          items={items}
          choose={item => {
            choose(item.id);
            setLibrary(false);
          }}
          onMenu={menu.open}
        />
        {modules && !modules.factors.length && <p className="subtle">{t("还没有因子")}</p>}
        <NewModule
          key={menu.copying?.id ?? "new"}
          folder="factors"
          repository={repository}
          copying={menu.copying}
          cancelCopy={menu.clearCopy}
          created={(path, id) => {
            setLibrary(false);
            setReport("");
            if (id) setChosen(id);
            source.open(path);
            void read();
          }}
        />
        <ModuleErrors
          folder="factors"
          modules={modules}
          contextTarget={menu.target}
          open={path => {
            setLibrary(false);
            setReport("");
            source.open(path);
          }}
          onMenu={menu.open}
        />
        {menu.overlay}
      </aside>
      {report ? (
        <div className="lab-center lab-runs-view">
          <header className="lab-report-header">
            <button type="button" className="lab-link" onClick={() => setReport("")}>
              {library ? t("← 因子库") : t("← 源码")}
            </button>
            <h2>{recordName(report)}</h2>
            <span className="lab-spacer" />
            <DeleteRecord name={report} remove={() => remove(report)} />
          </header>
          <Report key={report} id={report} />
        </div>
      ) : library ? (
        <FactorLibrary
          items={shown}
          total={items.length}
          chosen={chosen}
          selected={activeSelected}
          setSelected={setSelected}
          latest={latest}
          strategyUsers={strategyUsers}
          busy={busy || Boolean(context.busy)}
          batchProgress={batchProgress}
          error={error}
          onBatchEvaluate={() => void evaluateBatch()}
          onCompositeEvaluate={() => void evaluateComposite()}
          open={id => {
            choose(id);
            setLibrary(false);
          }}
        />
      ) : (
        <LabCentre
          source={source}
          repository={repository}
          modules={modules}
          changed={() => void read()}
          onDelete={path => menu.remove(path)}
        />
      )}
      {library ? (
        <aside className="lab-config" aria-label={t("批量与合成")}>
          <h2>
            {!folded && t("批量与合成")}
            <ConfigFold folded={folded} fold={setFolded} />
          </h2>
          {!folded && (
            <>
              {sharedSettings}
              <Section title={t("评价设置")} open>
                {evaluationFields}
                {combinationField}
              </Section>
              {composites.length > 0 && (
                <Section title={t("多因子合成记录")} open>
                  <ul className="lab-evaluations" aria-label={t("多因子合成记录")}>
                    {composites.map(item => {
                      const ic = item.horizons[0]?.mean;
                      const names = item.factorNames.length
                        ? item.factorNames.map(name => name.split(".")[1] ?? name).join(" + ")
                        : recordName(item.id);
                      return (
                        <li key={item.id}>
                          <button
                            type="button"
                            aria-pressed={item.id === report}
                            title={item.factorNames.join(" + ") || item.id}
                            onClick={() => setReport(item.id)}
                          >
                            <span>{names}</span>
                            <small>
                              {t("{count} 个因子", {
                                count:
                                  item.factorNames.length ||
                                  1 + (item.evaluation?.extraFactors.length ?? 0),
                              })}
                            </small>
                            <span className={`numeric ${signed(ic)}`}>{metric(ic, false)}</span>
                          </button>
                        </li>
                      );
                    })}
                  </ul>
                </Section>
              )}
              {saved.length > 0 && (
                <p className="subtle">
                  {t("已保存 {files}", { files: saved.join(listSeparator()) })}
                </p>
              )}
            </>
          )}
        </aside>
      ) : (
        factor && (
          <aside className="lab-config" aria-label={t("评价")}>
            <h2>
              {!folded && t("评价")}
              <ConfigFold folded={folded} fold={setFolded} />
            </h2>
            {!folded && (
              <>
                {sharedSettings}
                {factor.parameters.length > 0 && (
                  <Section title={t("参数")} open>
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
                  </Section>
                )}
                <Section title={t("评价设置")} open>
                  {evaluationFields}
                  {(modules?.factors.length ?? 0) > 1 && (
                    <>
                      {combinationField}
                      {(modules?.factors ?? [])
                        .filter(other => other.id !== factor.id)
                        .map(other => {
                          const checked = (form.extraFactors ?? []).includes(other.id);
                          return (
                            <label key={other.id} className="checkbox wide">
                              <input
                                type="checkbox"
                                checked={checked}
                                onChange={event => {
                                  const current = form.extraFactors ?? [];
                                  change({
                                    extraFactors: event.target.checked
                                      ? [...current, other.id]
                                      : current.filter(id => id !== other.id),
                                  });
                                }}
                              />
                              {other.id}
                            </label>
                          );
                        })}
                    </>
                  )}
                </Section>
                {users.length > 0 && (
                  <Section title={t("导入本模块的策略")} open>
                    <ul className="lab-evaluations" aria-label={t("导入本模块的策略")}>
                      {users.map(item => (
                        <li key={item.id}>
                          <button
                            type="button"
                            onClick={() => {
                              setLibrary(false);
                              setReport("");
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
                {evaluations.length > 0 && (
                  <Section title={t("评价记录")} open>
                    <ul className="lab-evaluations" aria-label={t("评价记录")}>
                      {evaluations.map(item => {
                        const ic = item.horizons[0]?.mean;
                        const label = isCompositeResult(item)
                          ? item.factorNames.map(name => name.split(".")[1] ?? name).join(" + ")
                          : Object.entries(item.evaluation?.params ?? {})
                              .map(([name, value]) => `${name}=${String(value.value.value)}`)
                              .join(" ") || recordName(item.id);
                        return (
                          <li key={item.id}>
                            <button
                              type="button"
                              aria-pressed={item.id === report}
                              title={item.id}
                              onClick={() => setReport(item.id)}
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
                <footer className="lab-submit">
                  <button
                    type="button"
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
                  <ActionError error={error} namespace="asterion.terminal.research" />
                </footer>
              </>
            )}
          </aside>
        )
      )}
    </section>
  );
}

type LibraryKey = "ic" | "holdoutIc" | "ir" | "t" | "turnover" | "strategies" | "evaluated";

// Every factor with its latest evaluation, multi-select batch & composite actions,
// and sortable figures (IC, holdout IC, ICIR, t, turnover, strategy references).
function FactorLibrary({
  items,
  total,
  chosen,
  selected,
  setSelected,
  latest,
  strategyUsers,
  busy,
  batchProgress,
  error,
  onBatchEvaluate,
  onCompositeEvaluate,
  open,
}: {
  items: readonly ModuleItem[];
  total: number;
  chosen: string;
  selected: readonly string[];
  setSelected: (next: string[] | ((previous: string[]) => string[])) => void;
  latest: (id: string) => FactorResult | undefined;
  strategyUsers: (id: string) => readonly StrategyEntry[];
  busy: boolean;
  batchProgress: { done: number; total: number } | null;
  error: DisplayError;
  onBatchEvaluate: () => void;
  onCompositeEvaluate: () => void;
  open: (id: string) => void;
}) {
  const [sort, setSort] = useState<{ key: LibraryKey; descending: boolean }>({
    key: "ic",
    descending: true,
  });
  const [announcement, setAnnouncement] = useState("");
  const selectAllRef = useRef<HTMLInputElement>(null);
  const shownIds = items.map(item => item.id);
  const selectedShown = shownIds.filter(id => selected.includes(id));
  const allShown = shownIds.length > 0 && selectedShown.length === shownIds.length;
  const someShown = selectedShown.length > 0 && !allShown;
  useEffect(() => {
    if (selectAllRef.current) selectAllRef.current.indeterminate = someShown;
  }, [someShown]);

  const value = (id: string) => {
    const result = latest(id);
    const first = result?.horizons[0];
    return {
      ic: first?.mean,
      holdoutIc: result?.holdoutHorizons[0]?.mean,
      ir: first?.ir,
      t: first?.t,
      turnover: factorTurnover(result),
      strategies: strategyUsers(id).length,
      evaluated: result ? Number(result.evaluatedMs) : undefined,
    }[sort.key];
  };
  const rows = [...items].sort((a, b) => {
    const [x, y] = [value(a.id), value(b.id)];
    if (x === undefined || y === undefined) return x === undefined ? (y === undefined ? 0 : 1) : -1;
    return sort.descending ? y - x : x - y;
  });
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
  const heading = (key: LibraryKey, label: string, title?: string) => (
    <th
      scope="col"
      className="numeric"
      title={title}
      aria-sort={sort.key === key ? (sort.descending ? "descending" : "ascending") : undefined}
    >
      <button
        type="button"
        className="lab-sort"
        onClick={() => {
          const descending = sort.key === key ? !sort.descending : true;
          setSort({ key, descending });
          setAnnouncement(
            t("已按 {column}{direction}排序", {
              column: label,
              direction: descending ? t("降序") : t("升序"),
            }),
          );
        }}
      >
        {label}
        <span aria-hidden="true">{sort.key === key ? (sort.descending ? " ↓" : " ↑") : ""}</span>
      </button>
    </th>
  );
  return (
    <div className="lab-center lab-library-view">
      <div className="lab-toolbar">
        <button
          type="button"
          className="primary"
          disabled={busy || selected.length === 0}
          onClick={onBatchEvaluate}
        >
          {batchProgress
            ? t("正在评价 {done} / {total}…", batchProgress)
            : t("批量评价（{count}）", { count: selected.length })}
        </button>
        <button type="button" disabled={busy || selected.length < 2} onClick={onCompositeEvaluate}>
          {busy && !batchProgress
            ? t("正在评价…")
            : t("相关性与合成（{count}）", { count: selected.length })}
        </button>
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
      </div>
      <ActionError error={error} namespace="asterion.terminal.research" />
      <p className="lab-sr-only" role="status">
        {announcement}
      </p>
      {!rows.length && total > 0 ? (
        <p className="subtle">{t("没有匹配的模块")}</p>
      ) : (
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
                <th scope="col">{t("因子")}</th>
                <th scope="col">{t("模块")}</th>
                <th scope="col">{t("行情字段")}</th>
                <th scope="col">{t("说明")}</th>
                {heading("ic", t("IC 均值"), t("最近一次评价的第一个持有期"))}
                {heading("holdoutIc", t("留出期 IC"), t("最近一次评价的留出期第一个持有期"))}
                {heading("ir", "ICIR")}
                {heading("t", t("t 值"))}
                {heading("turnover", t("换手率"), t("各分组日均换手率均值"))}
                {heading("strategies", t("引用策略"), t("引用该因子模块的策略数量"))}
                {heading("evaluated", t("评价时间"))}
              </tr>
            </thead>
            <tbody>
              {rows.map(item => {
                const result = latest(item.id);
                const first = result?.horizons[0];
                const holdoutIc = result?.holdoutHorizons[0]?.mean;
                const turnover = factorTurnover(result);
                const refs = strategyUsers(item.id);
                const checked = selected.includes(item.id);
                return (
                  <tr
                    key={item.id}
                    className={checked ? "selected" : item.id === chosen ? "current" : undefined}
                    title={item.description}
                    onClick={() => open(item.id)}
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
                    <td className="subtle">{item.inputs?.join(listSeparator())}</td>
                    <td className="subtle lab-factor-desc">{item.description || "—"}</td>
                    <td className={`numeric ${signed(first?.mean)}`}>
                      {metric(first?.mean, false)}
                    </td>
                    <td className={`numeric ${signed(holdoutIc)}`}>{metric(holdoutIc, false)}</td>
                    <td className="numeric">{metric(first?.ir, false)}</td>
                    <td className="numeric">{metric(first?.t, false)}</td>
                    <td className="numeric">{metric(turnover, true)}</td>
                    <td
                      className={`numeric${refs.length === 0 ? " subtle" : ""}`}
                      title={refs.map(entry => entry.id.split(".")[1] ?? entry.id).join(", ")}
                    >
                      {refs.length}
                    </td>
                    <td className="subtle numeric">
                      {result
                        ? new Date(Number(result.evaluatedMs)).toLocaleString("sv-SE", {
                            timeZone: "Asia/Shanghai",
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
