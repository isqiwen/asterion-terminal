import { useEffect, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  getLocale,
  marketData,
  translate,
  type DisplayError,
  type MarketChange,
  type MessageValues,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);

export const changeText = (change: MarketChange) =>
  t("新增 {added} · 替换 {replaced} · 删除 {removed}", {
    added: change.added,
    replaced: change.replaced,
    removed: change.removed,
  });

// A source's versions, newest first, each with what it changed from the one
// before. Choosing one shows its summary and coverage instead of the current.
export function DatasetHistory({
  latest,
  viewed,
  onView,
}: {
  // The current version; the list is read again when it changes.
  latest: string;
  viewed: string;
  onView: (version: string) => void;
}) {
  const [versions, setVersions] = useState<MarketChange[]>([]);
  const [next, setNext] = useState("");
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void marketData({ case: "listDatasetVersions", value: {} })
      .then(page => {
        if (!active) return;
        setVersions(page.versions);
        setNext(page.nextVersion);
        setError("");
      })
      .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [latest]);
  const more = async () => {
    try {
      const page = await marketData({
        case: "listDatasetVersions",
        value: { afterVersion: next },
      });
      setVersions(current => [...current, ...page.versions]);
      setNext(page.nextVersion);
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  };
  if (!versions.length && !error) return null;
  return (
    <section className="data-card" aria-label={t("版本历史")}>
      <header>
        <h3>{t("版本历史")}</h3>
      </header>
      <ErrorNotice error={error} />
      <table className="data-table" aria-label={t("版本历史")}>
        <thead>
          <tr>
            <th>{t("时间")}</th>
            <th>{t("版本")}</th>
            <th>{t("与上一版相比")}</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {versions.map((item, index) => {
            const version = item.dataset!.version;
            const shown = (viewed || latest) === version;
            return (
              <tr key={version} aria-current={shown || undefined}>
                <td>
                  {new Date(Number(item.dataset!.createdMs)).toLocaleString(getLocale(), {
                    hour12: false,
                  })}
                </td>
                <td>
                  <code title={version}>{version.slice(0, 12)}</code>
                  {index === 0 && ` · ${t("当前")}`}
                </td>
                <td>{changeText(item)}</td>
                <td>
                  <button
                    type="button"
                    disabled={shown}
                    onClick={() => onView(index === 0 ? "" : version)}
                  >
                    {t(shown ? "正在查看" : "查看")}
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      {next && (
        <button type="button" onClick={() => void more()}>
          {t("更早的版本")}
        </button>
      )}
    </section>
  );
}
