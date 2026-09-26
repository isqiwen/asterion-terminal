import type { RequestClient } from "@asterion/runtime-client/requests";
import { useEffect, useRef, useState } from "react";

import type { Config } from "./Research";

export type ResearchDraft = {
  schema_version: 1;
  config: Config;
  selected_run_id: string;
};
type Document = {
  id: string;
  name: string;
  revision: number;
  content: ResearchDraft;
  updated_at: number;
};
type Workspace = { draft: Document | null; templates: Document[] };
const stable = (value: unknown) =>
  JSON.stringify(value, (_key, item) =>
    item && typeof item === "object" && !Array.isArray(item)
      ? Object.fromEntries(
          Object.keys(item)
            .sort()
            .map((key) => [key, item[key]]),
        )
      : item,
  );

export function ResearchWorkspace({
  api,
  accountEmail,
  connected,
  value,
  onRestore,
  onReady,
}: {
  api: RequestClient;
  accountEmail: string;
  connected: boolean;
  value: ResearchDraft;
  onRestore: (value: ResearchDraft) => Promise<void>;
  onReady: (ready: boolean) => void;
}) {
  const scoped = (path: string) =>
    `${path}?expected_account=${encodeURIComponent(accountEmail)}`;
  const [ready, setReady] = useState(false),
    [saving, setSaving] = useState(false),
    [busy, setBusy] = useState(false),
    [paused, setPaused] = useState(false);
  const [error, setError] = useState(""),
    [status, setStatus] = useState("读取研究草稿…");
  const [templates, setTemplates] = useState<Document[]>([]),
    [selected, setSelected] = useState(""),
    [name, setName] = useState("");
  const revision = useRef(0),
    saved = useRef(""),
    initialized = useRef(false),
    live = useRef(true);
  const restore = useRef(onRestore);
  restore.current = onRestore;
  const readiness = useRef(onReady);
  readiness.current = onReady;
  const current = useRef(value);
  current.current = value;
  const newCommand = useRef<{ signature: string; id: string } | null>(null);
  const signature = stable(value);
  useEffect(() => {
    live.current = true;
    return () => {
      live.current = false;
    };
  }, []);
  const loading = useRef(false);
  async function load() {
    if (loading.current) return;
    loading.current = true;
    setBusy(true);
    setError("");
    setReady(false);
    readiness.current(false);
    try {
      const result = await api.request<Workspace>(
        scoped("/research/workspace"),
      );
      if (!live.current) return;
      if (!result || !Array.isArray(result.templates) || !("draft" in result))
        throw new Error("研究草稿接口响应无效");
      setTemplates(result.templates);
      revision.current = result.draft?.revision || 0;
      if (result.draft) {
        await restore.current(result.draft.content);
        saved.current = stable(result.draft.content);
      } else saved.current = stable(current.current);
      if (!live.current) return;
      initialized.current = true;
      setPaused(false);
      setReady(true);
      readiness.current(true);
      setStatus(
        result.draft
          ? "已恢复本机草稿，请重新确认数据假设"
          : "尚无草稿，修改后自动保存",
      );
    } catch (e) {
      if (live.current) {
        setError(String(e));
        setStatus("草稿读取失败，未覆盖服务器数据");
      }
    } finally {
      loading.current = false;
      if (live.current) setBusy(false);
    }
  }
  useEffect(() => {
    if (connected && !initialized.current) void load();
  }, [api, connected]);
  async function save() {
    if (!ready || saving || busy || !connected) return;
    const content = current.current,
      sent = stable(content);
    setSaving(true);
    setError("");
    setStatus("正在保存草稿…");
    try {
      const result = await api.request<Document>(
        scoped("/research/workspace/draft"),
        { expected_revision: revision.current, content },
      );
      if (!live.current) return;
      revision.current = result.revision;
      saved.current = sent;
      setPaused(false);
      setStatus("草稿已保存到本机");
    } catch (e) {
      if (live.current) {
        setError(String(e));
        setPaused(true);
        setStatus(
          "草稿未保存，请重试；遇到多窗口冲突可先另存模板，再载入服务器草稿",
        );
      }
    } finally {
      if (live.current) setSaving(false);
    }
  }
  useEffect(() => {
    if (
      !ready ||
      !connected ||
      paused ||
      saving ||
      busy ||
      signature === saved.current
    )
      return;
    setStatus("有未保存的修改…");
    const timer = setTimeout(() => void save(), 800);
    return () => clearTimeout(timer);
  }, [signature, ready, connected, paused, saving, busy]);
  async function template(action: "new" | "update" | "load" | "delete") {
    const chosen = templates.find((t) => t.id === selected);
    if (action !== "new" && !chosen) return;
    setBusy(true);
    setError("");
    try {
      if (action === "load") {
        setReady(false);
        readiness.current(false);
        await restore.current(chosen!.content);
        setStatus("已载入模板，作为当前草稿保存；不会自动运行");
        setReady(true);
        readiness.current(true);
        return;
      }
      if (action === "delete") {
        await api.request(scoped(`/research/templates/${chosen!.id}/delete`), {
          expected_revision: chosen!.revision,
        });
        setSelected("");
        setName("");
      } else {
        const content = { ...current.current, selected_run_id: "" };
        const key = stable({ name, content });
        if (newCommand.current?.signature !== key)
          newCommand.current = { signature: key, id: crypto.randomUUID() };
        const id = action === "new" ? newCommand.current.id : chosen!.id;
        const result = await api.request<Document>(
          scoped(`/research/templates/${id}`),
          {
            name,
            content,
            expected_revision: action === "new" ? 0 : chosen!.revision,
          },
        );
        setSelected(result.id);
        setName(result.name);
      }
      const latest = await api.request<Workspace>(
        scoped("/research/workspace"),
      );
      setTemplates(latest.templates);
      newCommand.current = null;
    } catch (e) {
      setError(String(e));
    } finally {
      if (live.current) setBusy(false);
    }
  }
  const disabled = !connected || busy || saving;
  return (
    <section aria-label="研究草稿与模板">
      <p role="status">{status}</p>
      <button
        type="button"
        disabled={disabled || !ready}
        onClick={() => void save()}
      >
        立即保存草稿
      </button>
      <button type="button" disabled={disabled} onClick={() => void load()}>
        载入服务器草稿
      </button>
      <details>
        <summary>实验模板管理</summary>
        <div className="research-fields">
          <label>
            实验模板
            <select
              value={selected}
              disabled={disabled}
              onChange={(e) => {
                setSelected(e.target.value);
                setName(
                  templates.find((t) => t.id === e.target.value)?.name || "",
                );
              }}
            >
              <option value="">选择已保存模板</option>
              {templates.map((t) => (
                <option key={t.id} value={t.id}>
                  {t.name} · v{t.revision}
                </option>
              ))}
            </select>
          </label>
          <label>
            模板名称
            <input
              maxLength={80}
              value={name}
              disabled={disabled}
              onChange={(e) => setName(e.target.value)}
            />
          </label>
        </div>
        <button
          type="button"
          disabled={disabled || !ready || !name.trim()}
          onClick={() => void template("new")}
        >
          保存为新模板
        </button>
        <button
          type="button"
          disabled={disabled || !ready || !selected || !name.trim()}
          onClick={() => void template("update")}
        >
          更新所选模板
        </button>
        <button
          type="button"
          disabled={disabled || !selected}
          onClick={() => void template("load")}
        >
          载入模板
        </button>
        <button
          type="button"
          disabled={disabled || !selected}
          onClick={() => void template("delete")}
        >
          删除模板
        </button>
      </details>
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
