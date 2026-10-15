import { useState } from "react";
import type { ExperimentMetadata as Metadata, TaskRecord } from "../../src/bridge/client";
import {
  ErrorNotice,
  asDisplayError,
  useWorkspaceDraft,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
const namespace = "asterion.terminal.research";
const t = (key: string) => translate(namespace, key);
export const emptyMetadata: Metadata = {
  revision: 0,
  name: "",
  note: "",
  source_id: "",
  source_instance: "",
  source_result_digest: "",
  data_choice: "",
  snapshot: "",
};
export function ExperimentMetadata({
  task,
  connection,
  busy,
  trade,
  query,
}: { task: TaskRecord; connection: string } & Pick<TerminalContext, "busy" | "trade" | "query">) {
  const [confirmed, setConfirmed] = useState(task.metadata ?? emptyMetadata);
  const [draft, setDraft] = useWorkspaceDraft(`description:${connection}:${task.id}`, {
    name: confirmed.name,
    note: confirmed.note,
    revision: confirmed.revision,
  });
  const [pending, setPending] = useState(false),
    [saved, setSaved] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  async function save() {
    setPending(true);
    setSaved(false);
    setError("");
    try {
      await trade("task.metadata.edit", {
        id: task.id,
        expected_revision: draft.revision,
        name: draft.name,
        note: draft.note,
      });
      const revision = draft.revision + 1;
      setConfirmed({ ...confirmed, ...draft, revision });
      setDraft({ ...draft, revision });
      setSaved(true);
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setPending(false);
    }
  }
  async function reload() {
    setPending(true);
    setSaved(false);
    setError("");
    try {
      const reply = await query("task.metadata.read", { id: task.id });
      const metadata = reply.task_description?.metadata ?? emptyMetadata;
      setConfirmed(metadata);
      setDraft({ ...draft, revision: metadata.revision });
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setPending(false);
    }
  }
  return (
    <details>
      <summary>{t("名称、备注与来源")}</summary>
      <section aria-label={t("实验描述")}>
        <p>
          {t(
            "名称和备注只改变描述，不改变计算输入、任务 ID 或结果。保存失败时草稿保留；读取最新描述保留草稿，请对照已保存内容后再保存。",
          )}
        </p>
        <p>
          {t("已保存名称")}: {confirmed.name || "—"}
        </p>
        <p className="experiment-note">
          {t("已保存备注")}: {confirmed.note || "—"}
        </p>
        {confirmed.source_id ? (
          <div>
            <p>
              {t("来源实验")}:{" "}
              <code>
                {confirmed.source_instance} / {confirmed.source_id}
              </code>
            </p>
            <p>
              {t("来源结果摘要")}: <code>{confirmed.source_result_digest}</code>
            </p>
            <p>
              {t("派生数据选择")}:{" "}
              {t(confirmed.data_choice === "fixed" ? "沿用原固定快照" : "选择指定新快照")}
            </p>
            {confirmed.snapshot && (
              <p>
                {t("固定快照")}: <code>{confirmed.snapshot}</code>
              </p>
            )}
          </div>
        ) : (
          <p>{t("来源未记录，不推断既有实验的派生关系。")}</p>
        )}
        <form
          onSubmit={event => {
            event.preventDefault();
            void save();
          }}
        >
          <fieldset disabled={busy || pending}>
            <label>
              {t("实验名称")}
              <input
                maxLength={64}
                value={draft.name}
                onChange={event => {
                  setSaved(false);
                  setDraft({ ...draft, name: event.target.value });
                }}
              />
            </label>
            <label>
              {t("实验备注")}
              <textarea
                maxLength={2000}
                value={draft.note}
                onChange={event => {
                  setSaved(false);
                  setDraft({ ...draft, note: event.target.value });
                }}
              />
            </label>
            <button type="submit">{t("保存描述")}</button>
            <button type="button" onClick={() => void reload()}>
              {t("读取最新描述（保留草稿）")}
            </button>
          </fieldset>
        </form>
        {saved && <p role="status">{t("描述已保存")}</p>}
        <ErrorNotice error={error} namespace={namespace} />
      </section>
    </details>
  );
}
