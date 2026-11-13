import { DateInput } from "../../src/ui/DateInput";
import { useState } from "react";
import { lab } from "../../src/bridge/lab";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import {
  defaultFactorForm,
  defaultText,
  factorRequest,
  identity,
  parameterKind,
  type FactorForm,
} from "./research-lab";
import {
  ConfigFold,
  LabCentre,
  LabHeader,
  ModuleErrors,
  NewModule,
  Section,
  useConfigFold,
  useModules,
  useSource,
} from "./LabParts";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// The factor view: the factor modules, their code and an evaluation's
// settings. An evaluation opens its report in the evaluations view.
export function FactorLab(context: TerminalContext) {
  const repository = context.snapshot?.research_directory ?? "";
  const { modules, read } = useModules(repository);
  // Kept while an evaluation's report is on show in the evaluations view.
  const [chosen, setChosen] = useWorkspaceDraft(`factor:${repository}`, "");
  const [form, setForm] = useWorkspaceDraft<FactorForm>("factor-form", defaultFactorForm);
  const [busy, setBusy] = useState(false);
  const { error, attempt } = useActionError();
  const source = useSource(repository);
  const [saved, setSaved] = useState<string[]>([]);

  const factor = modules?.factors.find(item => item.id === chosen);
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
      context.navigate("workspace.lab-evaluations", { params: { id } });
    });
    setBusy(false);
  };
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
        <h4>{t("因子")}</h4>
        <ul>
          {modules?.factors.map(item => {
            const [module, name] = item.id.split(".");
            return (
              <li key={item.id}>
                <button
                  aria-pressed={item.id === chosen}
                  onClick={() => {
                    setChosen(item.id);
                    source.open(`factors/${module}.py`);
                    change({
                      params: Object.fromEntries(
                        item.parameters.map(p => [p.name, defaultText(p)]),
                      ),
                    });
                  }}
                >
                  <strong>{name}</strong>
                  <small>
                    {module} · {item.inputs.join(listSeparator())}
                  </small>
                  {item.description && (
                    <span>{item.description.split("\n")[0].replace(/`/g, "")}</span>
                  )}
                </button>
              </li>
            );
          })}
        </ul>
        {modules && !modules.factors.length && (
          <p className="subtle">{t("factors/ 里还没有因子函数。")}</p>
        )}
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
      <LabCentre
        source={source}
        repository={repository}
        changed={() => void read()}
        deleted={() => {
          setChosen("");
          void read();
        }}
      >
        <p className="lab-placeholder">
          {t("选择左侧的因子，填写右侧的设置后评价；报告在“因子评价”里。")}
        </p>
      </LabCentre>
      <aside className="lab-config">
        <h4>
          {!folded && t("因子评价")}
          <ConfigFold folded={folded} fold={setFolded} />
        </h4>
        {!folded && (
          <>
            {!factor ? (
              <p className="subtle">{t("先选择一个因子。")}</p>
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
