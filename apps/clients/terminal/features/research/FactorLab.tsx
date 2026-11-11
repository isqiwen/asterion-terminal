import { useCallback, useEffect, useState } from "react";
import {
  ActionError,
  lab,
  useActionError,
  translate,
  getLocale,
  notify,
  type LabFactorResult,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import {
  defaultFactorForm,
  defaultText,
  factorRequest,
  identity,
  parameterKind,
  type FactorForm,
} from "./research-lab";
import { FactorResult } from "./FactorResult";
import { LabCentre, LabHeader, ModuleErrors, NewModule, useModules, useSource } from "./LabParts";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// The factor view: the factor modules, an evaluation's settings and its result.
export function FactorLab(context: TerminalContext) {
  const repository = context.snapshot?.research_directory ?? "";
  const { modules, read } = useModules(repository);
  const [chosen, setChosen] = useState("");
  const [form, setForm] = useState<FactorForm>(defaultFactorForm);
  const [result, setResult] = useState<LabFactorResult | null>(null);
  // Every kept evaluation, newest first, without its days and groups.
  const [kept, setKept] = useState<LabFactorResult[]>([]);
  const readKept = useCallback(async () => {
    try {
      setKept((await lab({ case: "listFactorResults", value: {} })).results);
    } catch (failure) {
      notify(failure);
    }
  }, []);
  useEffect(() => void readKept(), [readKept]);
  const open = (id: string) =>
    attempt(async () => setResult(await lab({ case: "getFactorResult", value: { id } })));
  const [busy, setBusy] = useState(false);
  const { error, attempt } = useActionError();
  const source = useSource(repository, result);
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
      setResult(
        await lab({
          case: "evaluateFactor",
          value: { ...factorRequest(form, repository, factor), id: identity(factor.id) },
        }),
      );
      await readKept();
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

  return (
    <section className="lab-research lab-factors">
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
                    {module} · {item.inputs.join(", ")}
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
        hasResult={!!result}
        changed={() => void read()}
        deleted={() => {
          setChosen("");
          void read();
        }}
      >
        {result ? (
          <FactorResult result={result} />
        ) : (
          <p className="lab-placeholder">
            {t("选择左侧的因子、填写右侧的设置后评价；结果显示在这里。")}
          </p>
        )}
      </LabCentre>
      <aside className="lab-config">
        <h4>{t("因子评价")}</h4>
        {!factor ? (
          <p className="subtle">{t("先选择一个因子。")}</p>
        ) : (
          <>
            <div className="lab-form">
              <label className="wide">
                <span>{t("品种")}</span>
                <input
                  value={form.symbols}
                  placeholder={t("SHFE（整个交易所）或 SHFE/rb, SHFE/au")}
                  onChange={event => change({ symbols: event.target.value })}
                />
              </label>
              {field("开始日", "begin", { type: "date" })}
              {field("结束日", "end", { type: "date" })}
              {factor.parameters.length > 0 && <h5 className="wide">{t("参数")}</h5>}
              {factor.parameters.map(parameter => (
                <label key={parameter.name}>
                  <span>
                    {parameter.name} <small>{parameterKind(parameter)}</small>
                  </span>
                  <input
                    value={form.params[parameter.name] ?? ""}
                    onChange={event =>
                      change({ params: { ...form.params, [parameter.name]: event.target.value } })
                    }
                  />
                </label>
              ))}
              <h5 className="wide">{t("评价")}</h5>
              {field("持有期（K 线数）", "horizons")}
              {field("分组数", "groups")}
            </div>
            <footer className="lab-submit">
              <button
                className="primary"
                disabled={busy || context.busy}
                onClick={() => void evaluate()}
              >
                {busy ? t("正在评价…") : t("评价")}
              </button>
              {saved.length > 0 && (
                <p className="subtle">{t("已保存 {files}", { files: saved.join("、") })}</p>
              )}
              <ActionError error={error} />
            </footer>
          </>
        )}
      </aside>
      <footer className="lab-runs">
        <header>
          <h4>{t("评价记录")}</h4>
        </header>
        <table className="lab-table">
          <thead>
            <tr>
              <th>{t("名称")}</th>
              <th>{t("因子")}</th>
              <th>{t("区间")}</th>
              <th>{t("评价时间")}</th>
            </tr>
          </thead>
          <tbody>
            {kept.map(item => (
              <tr key={item.id} className={item.id === result?.id ? "current" : undefined}>
                <td>
                  <button className="lab-link strong" onClick={() => void open(item.id)}>
                    {item.id}
                  </button>
                </td>
                <td className="subtle">{item.evaluation?.factor}</td>
                <td className="subtle">
                  {item.evaluation?.firstDay} – {item.evaluation?.lastDay}
                </td>
                <td className="subtle numeric-left">
                  {new Date(Number(item.evaluatedMs)).toLocaleString(getLocale())}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </footer>
    </section>
  );
}
