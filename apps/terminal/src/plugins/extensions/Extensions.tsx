import { useEffect, useRef, useState } from "react";
import "./extensions.css";
import { Diagnostics } from "./Diagnostics";
import type { RequestClient } from "../../api/requests";

const capabilityLabels: Record<string, string> = { "data.provider": "数据源", "research.strategy": "研究策略", "ui.table": "表格视图" };

type Installed = {
  manifest: { id: string; title: string; version: string; description: string; requires: Record<string, string>; contributions: Record<string, unknown> };
  digest: string;
  enabled: boolean;
};

function encoded(file: File): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error("无法读取插件包"));
    reader.onload = () => resolve(String(reader.result).split(",")[1]);
    reader.readAsDataURL(file);
  });
}

export function Extensions({ api }: { api: RequestClient }) {
  const [items, setItems] = useState<Installed[]>([]);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const inFlight = useRef(false);
  const [dragging, setDragging] = useState(false);
  const [pending, setPending] = useState<(Omit<Installed, "enabled"> & { archive: string }) | null>(null);
  const [notice, setNotice] = useState("");
  const [approval, setApproval] = useState<Installed | null>(null);
  const refresh = async () => {
    const result = await api.request<{ items: Installed[] }>("/extensions");
    setItems(result.items);
  };
  useEffect(() => {
    let active = true;
    api.request<{ items: Installed[] }>("/extensions").then(
      (value) => { if (active) setItems(value.items); },
      (reason) => { if (active) setError(String(reason)); },
    );
    return () => { active = false; };
  }, [api]);
  async function perform(action: () => Promise<unknown>) {
    if (inFlight.current) return;
    inFlight.current = true;
    setBusy(true); setError("");
    try { await action(); setApproval(null); await refresh(); }
    catch (reason) { setError(String(reason)); }
    finally { inFlight.current = false; setBusy(false); }
  }
  useEffect(() => {
    const preventFileNavigation = (event: DragEvent) => {
      if (Array.from(event.dataTransfer?.types ?? []).includes("Files")) event.preventDefault();
    };
    window.addEventListener("dragover", preventFileNavigation);
    window.addEventListener("drop", preventFileNavigation);
    return () => {
      window.removeEventListener("dragover", preventFileNavigation);
      window.removeEventListener("drop", preventFileNavigation);
    };
  }, []);
  async function inspectFiles(files: File[]) {
    setDragging(false);
    if (inFlight.current) return;
    setPending(null); setApproval(null); setNotice(""); setError("");
    if (files.length !== 1) { setError("请一次选择一个 ZIP 插件包"); return; }
    const file = files[0];
    if (!file.name.toLowerCase().endsWith(".zip")) { setError("请选择 ZIP 插件包，不支持文件夹或其他文件格式"); return; }
    if (file.size === 0) { setError("插件包为空，请重新选择文件"); return; }
    if (file.size > 16_000_000) { setError("插件包超过 16 MB"); return; }
    inFlight.current = true; setBusy(true);
    try {
      const archive = await encoded(file);
      const result = await api.request<Omit<Installed, "enabled">>("/extensions/inspect", { archive });
      setPending({ ...result, archive });
    } catch (reason) { setError(String(reason)); }
    finally { inFlight.current = false; setBusy(false); }
  }
  async function toggle(item: Installed, enabled: boolean) {
    await perform(() => api.request(`/extensions/${item.manifest.id}/state`, {
      digest: item.digest, enabled, trust_local_code: enabled,
    }));
  }
  return <>
    <h2 className="settings-section-title">本地插件</h2>
    <p className="settings-note">选择插件包，核对后即可安装并启用，无需命令行或单独安装运行环境。</p>
    <section role="region" aria-label="插件包拖放区域" aria-busy={busy}
      className={`plugin-drop-zone${dragging && !busy ? " is-dragging" : ""}${busy ? " is-busy" : ""}`}
      onDragOver={(event) => {
        if (!event.dataTransfer.types.includes("Files")) return;
        event.preventDefault();
        event.dataTransfer.dropEffect = busy ? "none" : "copy";
        if (!busy) setDragging(true);
      }}
      onDragLeave={(event) => {
        if (!(event.relatedTarget instanceof Node) || !event.currentTarget.contains(event.relatedTarget)) setDragging(false);
      }}
      onDrop={(event) => {
        event.preventDefault();
        void inspectFiles(Array.from(event.dataTransfer.files));
      }}>
      <label className="setting-row">
        <span>{busy ? "正在处理插件包…" : dragging ? "松开以检查插件包" : "拖入插件包，或选择文件"}<small>一次一个 ZIP · 最大 16 MB · 确认后才会安装</small></span>
        <input aria-label="选择插件包" type="file" accept=".zip" disabled={busy} onChange={(event) => {
          const files = Array.from(event.target.files ?? []); event.target.value = "";
          if (files.length) void inspectFiles(files);
        }} />
      </label>
    </section>
    {error && <div role="alert" className="alert">{error}</div>}
    {notice && <p role="status" className="settings-note">{notice}</p>}
    {pending && <section role="dialog" aria-label="确认安装插件" className="alert">
      <h3>{pending.manifest.title} · {pending.manifest.version}</h3>
      <p>{pending.manifest.description}</p>
      <p>提供功能：{Object.keys(pending.manifest.contributions).map((kind) => capabilityLabels[kind] ?? kind).join("、")}</p>
      <p>这是你选择的本地代码，来源未经认证。启用后拥有当前用户的文件和网络访问权限，请确认来源可信。</p>
      {Object.keys(pending.manifest.requires).length > 0 && <p>依赖已检查：{Object.entries(pending.manifest.requires).map(([id, version]) => `${id} ${version}`).join("、")}</p>}
      <details><summary>技术详情</summary><p>{pending.manifest.id}</p><p>内容摘要：{pending.digest}</p></details>
      <p><button disabled={busy} onClick={() => void perform(async () => {
        const selected = pending;
        await api.request("/extensions/install", { archive: selected.archive, digest: selected.digest, trust_local_code: true });
        setPending(null);
        const steps = [];
        if ("data.provider" in selected.manifest.contributions) steps.push("在“数据源”中配置连接");
        if ("research.strategy" in selected.manifest.contributions) steps.push("在研究面板刷新策略列表");
        if ("ui.table" in selected.manifest.contributions) steps.push("在“扩展”中打开插件视图");
        setNotice(`${selected.manifest.title} 已安装并启用。${steps.join("；")}。`);
      })}>{busy ? "正在安装…" : "信任并安装启用"}</button>{" "}
      <button disabled={busy} onClick={() => setPending(null)}>取消</button></p>
    </section>}

    {items.length === 0 && <p className="settings-note">尚未安装本地插件。内置功能随终端交付。</p>}
    {items.map((item) => <div key={item.manifest.id} className="setting-row">
      <div>{item.manifest.title} <span className="muted">{item.manifest.version}</span>
        <small>{item.manifest.description}</small>
        <small title={item.digest}>{item.manifest.id} · {item.digest.slice(0, 12)}</small>
        <Diagnostics key={item.digest} api={api} identifier={item.manifest.id} />
      </div>
      <div>
        <span className={item.enabled ? "good" : "muted"}>{item.enabled ? "已启用" : "已停用"}</span>{" "}
        <button disabled={busy} onClick={() => item.enabled ? void toggle(item, false) : setApproval(item)}>{item.enabled ? "停用" : "启用"}</button>{" "}
        <button disabled={busy || item.enabled} title="移除安装记录，保留已引用的工件和数据" onClick={() => void perform(() => api.request(`/extensions/${item.manifest.id}/remove`, { digest: item.digest }))}>移除</button>
      </div>
    </div>)}
    {approval && <section role="dialog" aria-label="信任并启用插件" className="alert">
      <p>启用 {approval.manifest.title}？该代码拥有当前用户的本机访问权限。请确认插件来源及内容可信。</p>
      <small>工件：{approval.digest}</small>
      <p><button disabled={busy} onClick={() => void toggle(approval, true)}>信任并启用</button>{" "}
        <button disabled={busy} onClick={() => setApproval(null)}>取消</button></p>
    </section>}
    <p className="settings-note">启用数据源插件后，在“数据源”中配置连接；启用策略插件后，在研究面板点击“刷新策略”并选择策略。停用会阻止后续调用；已开始的调用有执行时限。移除插件不会删除数据或工件。</p>
  </>;
}
