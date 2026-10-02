import { useEffect, useRef, useState } from "react";
import type { SavedResearchDataset, Snapshot, TerminalCommand } from "../bridge/client";
import { translate } from "../i18n";
import { asDisplayError, ErrorNotice, type DisplayError } from "../i18n/errors";
import type { HistoryQuery } from "./useHistoryDatasets";
const t = (key: string) => translate("host", key);
export function SavedDatasets({
  snapshot,
  busy,
  query,
  trade,
  onSelected,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  query: HistoryQuery;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
  onSelected?: () => void;
}) {
  const [items, setItems] = useState<SavedResearchDataset[]>([]);
  const [id, setId] = useState("");
  const [name, setName] = useState("");
  const [working, setWorking] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [saved, setSaved] = useState(false);
  const reads = useRef({ sequence: 0 });
  const api = useRef(query);
  api.current = query;
  const online = !!snapshot?.research?.online;
  const selected = items.find(item => item.id === id);
  useEffect(() => {
    let active = true;
    const counter = reads.current;
    const request = ++reads.current.sequence;
    setItems([]);
    setId("");
    setError("");
    if (online)
      void api
        .current("research.dataset.saved", {})
        .then(result => {
          if (active && request === reads.current.sequence) setItems(result.saved_datasets ?? []);
        })
        .catch(reason => {
          if (active && request === reads.current.sequence) setError(asDisplayError(reason));
        });
    return () => {
      active = false;
      ++counter.sequence;
    };
  }, [online]);
  async function action(kind: "refresh" | "save" | "use") {
    const request = ++reads.current.sequence;
    setWorking(true);
    setError("");
    setSaved(false);
    try {
      if (kind === "use") {
        await trade("research.dataset.use", { id });
        onSelected?.();
      } else {
        if (kind === "save") await trade("research.dataset.save", { name: name.trim() });
        const result = await query("research.dataset.saved", {});
        if (request !== reads.current.sequence) return;
        setItems(result.saved_datasets ?? []);
        if (kind === "save") {
          setSaved(true);
          setName("");
        }
      }
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setWorking(false);
    }
  }
  return (
    <section className="saved-datasets" aria-label={t("已保存数据集")}>
      <fieldset disabled={busy || working || !online}>
        <div className="saved-dataset-row">
          <label>
            {t("已保存数据集")}
            <select
              aria-label={t("已保存数据集")}
              value={id}
              onChange={event => setId(event.target.value)}
            >
              <option value="">{t("选择已保存数据集")}</option>
              {items.map(item => (
                <option key={item.id} value={item.id}>
                  {item.name} · {item.selections.length} {t("个合约")} · {item.id.slice(0, 8)}
                </option>
              ))}
            </select>
          </label>
          <button type="button" disabled={!selected} onClick={() => void action("use")}>
            {t("使用已保存数据集")}
          </button>
          <button type="button" onClick={() => void action("refresh")}>
            {t("刷新")}
          </button>
        </div>
        {selected && (
          <p className="subtle">
            {selected.selections
              .map(
                input =>
                  `${input.contract.venue} · ${input.contract.symbol} · ${!input.begin_day && !input.end_day ? t("全部交易日") : `${input.begin_day || "…"} — ${input.end_day || "…"}`}`,
              )
              .join("； ")}
          </p>
        )}
        {selected && !!snapshot?.datasets.length && (
          <p className="subtle">{t("使用后替换当前选择，已有任务不受影响。")}</p>
        )}
        {!!snapshot?.datasets.length && (
          <details>
            <summary>{t("保存当前选择")}</summary>
            <div className="saved-dataset-row">
              <label>
                {t("数据集名称")}
                <input
                  aria-label={t("数据集名称")}
                  value={name}
                  maxLength={40}
                  onKeyDown={event => {
                    if (event.key === "Enter") {
                      event.preventDefault();
                      if (name.trim() && !busy && !working && online) void action("save");
                    }
                  }}
                  onChange={event => {
                    setName(event.target.value);
                    setSaved(false);
                  }}
                />
              </label>
              <button type="button" disabled={!name.trim()} onClick={() => void action("save")}>
                {t("保存数据集")}
              </button>
            </div>
            <p className="subtle">
              {t("保存合约、区间、计量参数和固定数据版本；同名不同输入另存为独立版本。")}
            </p>
          </details>
        )}
      </fieldset>
      {saved && <p role="status">{t("数据集已保存，可在当前数据服务中重复使用。")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
