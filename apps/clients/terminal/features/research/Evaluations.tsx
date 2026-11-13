import { useCallback, useEffect, useState } from "react";
import { lab } from "../../src/bridge/lab";
import type { FactorResult } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, getLocale, translate } from "../../src/i18n";
import { type DisplayError, ErrorNotice, asDisplayError } from "../../src/i18n/errors";
import { FactorResultView } from "./FactorResultView";
import { DeleteRecord } from "./LabParts";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// Every kept factor evaluation, and each one's report. The factor view opens a
// new one: param `id`.
export function Evaluations(context: TerminalContext) {
  // Newest first, without their days and groups.
  const [kept, setKept] = useState<FactorResult[] | null>(null);
  const [shown, setShown] = useState("");
  const [search, setSearch] = useWorkspaceDraft("evaluations-search", "");
  const acted = useActionError();
  const opened = context.workspaceParams?.id;
  useEffect(() => {
    if (opened) setShown(opened);
  }, [opened]);
  const read = useCallback(async () => {
    try {
      setKept((await lab({ case: "listFactorResults", value: {} })).results);
    } catch (failure) {
      notify(failure);
    }
  }, []);
  useEffect(() => void read(), [read]);
  const remove = (id: string) =>
    void acted.attempt(async () => {
      await lab({ case: "deleteFactorResult", value: { id } });
      setShown("");
      await read();
    });

  if (shown)
    return (
      <section className="lab-runs-view">
        <header className="lab-report-header">
          <button className="lab-link" onClick={() => setShown("")}>
            {t("← 评价记录")}
          </button>
          <h3>{shown}</h3>
          <span className="lab-spacer" />
          <DeleteRecord name={shown} remove={() => remove(shown)} />
        </header>
        <ActionError error={acted.error} />
        <Report key={shown} id={shown} />
      </section>
    );
  const query = search.trim().toUpperCase();
  const rows = (kept ?? []).filter(
    item => !query || `${item.id} ${item.evaluation?.factor}`.toUpperCase().includes(query),
  );
  return (
    <section className="lab-runs-view">
      <div className="lab-toolbar">
        <input
          type="search"
          aria-label={t("搜索名称或因子")}
          placeholder={t("搜索名称或因子")}
          value={search}
          onChange={event => setSearch(event.target.value)}
        />
      </div>
      <ActionError error={acted.error} />
      {!kept ? (
        <p className="subtle" role="status">
          {t("读取中")}
        </p>
      ) : !rows.length ? (
        <p className="lab-placeholder">
          {kept.length ? t("没有匹配的评价。") : t("还没有评价。在“因子”里选择因子并评价。")}
        </p>
      ) : (
        <div className="lab-scroll">
          <table className="lab-table lab-records" aria-label={t("评价记录")}>
            <thead>
              <tr>
                <th>{t("名称")}</th>
                <th>{t("因子")}</th>
                <th>{t("参数")}</th>
                <th>{t("区间")}</th>
                <th>{t("评价时间")}</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {rows.map(item => (
                <tr key={item.id}>
                  <td>
                    <button className="lab-link strong" onClick={() => setShown(item.id)}>
                      {item.id}
                    </button>
                  </td>
                  <td className="subtle">{item.evaluation?.factor}</td>
                  <td className="subtle lab-params">
                    {Object.entries(item.evaluation?.params ?? {})
                      .map(([name, value]) => `${name}=${String(value.value.value)}`)
                      .join(" ")}
                  </td>
                  <td className="subtle">
                    {item.evaluation?.firstDay} – {item.evaluation?.lastDay}
                  </td>
                  <td className="subtle numeric-left">
                    {new Date(Number(item.evaluatedMs)).toLocaleString(getLocale(), {
                      hour12: false,
                    })}
                  </td>
                  <td>
                    <DeleteRecord name={item.id} remove={() => remove(item.id)} />
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
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
