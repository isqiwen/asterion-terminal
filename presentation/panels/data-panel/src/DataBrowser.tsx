import { useEffect, useMemo, useState } from "react";
import type { RequestClient } from "@asterion/runtime-client/requests";
import type { components } from "@asterion/api-types/schema";
import { RoleDiagnostics } from "@asterion/ui-role-panel/RoleDiagnostics";
import { DatasetCatalog } from "./DatasetCatalog";
import type { ComponentProps } from "react";
import "./hierarchy.css";

type Node = components["schemas"]["DirectoryNode"];
type RoleNode = components["schemas"]["RoleDirectory"];
const exchanges: Record<string, string> = { SHFE: "SHFE · 上期所", DCE: "DCE · 大商所", CZCE: "CZCE · 郑商所", CFFEX: "CFFEX · 中金所", INE: "INE · 能源中心", GFEX: "GFEX · 广期所" };
const key = (path: string[]) => path.join("/");

export function DataBrowser({ rolesApi, ...props }: ComponentProps<typeof DatasetCatalog> & { rolesApi: RequestClient }) {
  const [expanded, setExpanded] = useState<Set<string>>(() => new Set());
  const [nodes, setNodes] = useState<Node[]>([]);
  const [path, setPath] = useState<string[]>([]);
  const [errors, setErrors] = useState<string[]>([]);
  const [loading, setLoading] = useState(false);
  const [refresh, setRefresh] = useState(0);
  const [archived, setArchived] = useState(false);
  useEffect(() => {
    setNodes([]); setErrors([]);
    if (!props.connected) return;
    let live = true;
    let generation = 0;
    async function read() {
      const request = ++generation;
      setLoading(true);
      const results = await Promise.allSettled([
        props.api.request<Node[]>(`/data/hierarchy?include_archived=${archived}`),
        rolesApi.request<RoleNode[]>("/contract-roles/hierarchy"),
      ]);
      if (!live || request !== generation) return;
      const merged = new Map<string, Node>();
      const problems: string[] = [];
      if (results[0].status === "fulfilled") for (const node of results[0].value) merged.set(key(node.path), node);
      else problems.push(`数据目录读取失败：${String(results[0].reason)}`);
      if (results[1].status === "fulfilled") for (const row of results[1].value) {
        const [exchange, product] = row.product_id.split(".");
        const branch = [exchange, product, "roles", row.role];
        const labels = [exchanges[exchange] ?? exchange, product, "角色映射", row.role === "main" ? "主力" : "次主力"];
        branch.forEach((_, i) => {
          const path = branch.slice(0, i + 1);
          if (!merged.has(key(path))) merged.set(key(path), {path, label: labels[i], count: row.count});
        });
      } else problems.push(`角色目录读取失败：${String(results[1].reason)}`);
      setNodes([...merged.values()].sort((a, b) => key(a.path).localeCompare(key(b.path))));
      setErrors(problems); setLoading(false);
    }
    void read();
    const update = () => void read();
    window.addEventListener("asterion:catalog-changed", update);
    window.addEventListener("asterion:unlocked", update);
    return () => { live = false; window.removeEventListener("asterion:catalog-changed", update); window.removeEventListener("asterion:unlocked", update); };
  }, [props.api, props.connected, rolesApi, archived, refresh]);
  const groups = useMemo(() => {
    const result = new Map<string, Node[]>();
    for (const node of nodes) {
      const parent = key(node.path.slice(0, -1));
      const siblings = result.get(parent) ?? [];
      siblings.push(node); result.set(parent, siblings);
    }
    return result;
  }, [nodes]);
  function children(parent: string[]): React.ReactNode {
    return (groups.get(key(parent)) ?? []).map(node => {
      const id = key(node.path), expandable = groups.has(id), open = expanded.has(id);
      return <li key={id}><div className="data-directory-row">
        {expandable && <button className="data-directory-toggle" aria-label={`${open ? "收起" : "展开"}${node.label}`} aria-expanded={open} onClick={() => setExpanded(previous => { const next = new Set(previous); if (open) next.delete(id); else next.add(id); return next; })}>{open ? "▾" : "▸"}</button>}
        <button aria-current={key(path) === id ? "location" : undefined} title={id} onClick={() => { setPath(node.path); if (expandable) setExpanded(previous => new Set(previous).add(id)); }}>{node.label}</button>
      </div>{expandable && open && <ul>{children(node.path)}</ul>}</li>;
    });
  }
  const rolePath = path[2] === "roles";
  return <div className="data-browser">
    <aside className="data-directory" aria-label="期货数据目录">
      <div className="panel-heading"><strong>期货</strong><button disabled={!props.connected || loading} onClick={() => setRefresh(v => v + 1)}>刷新目录</button></div>
      <button aria-current={!path.length ? "location" : undefined} onClick={() => setPath([])}>全部数据</button>
      {loading && <p role="status">正在读取目录…</p>}
      {errors.map(error => <p role="alert" key={error}>{error}</p>)}
      {!loading && !errors.length && !nodes.length && <p>暂无本地数据，请先获取数据。</p>}
      <ul>{children([])}</ul>
      <p className="panel-footnote">按已保存资料显示；交易所公共资料不归入单一品种。</p>
    </aside>
    <div className="data-directory-content">
      {!!path.length && <nav aria-label="数据位置" className="data-breadcrumb"><button onClick={() => setPath([])}>期货</button>{path.map((part, i) => <button key={i} onClick={() => setPath(path.slice(0, i + 1))}>{nodes.find(n => key(n.path) === key(path.slice(0, i + 1)))?.label ?? part}</button>)}</nav>}
      {rolePath ? <RoleDiagnostics key={key(path)} api={rolesApi} connected={props.connected} active productId={`${path[0]}.${path[1]}`} role={path[3] as "main" | "secondary" | undefined} /> : <DatasetCatalog {...props} initialVersionId={path.length ? undefined : props.initialVersionId} initialReportId={path.length ? undefined : props.initialReportId} key={key(path)} directory={key(path)} onArchivedChange={setArchived} includeArchivedDirectory={archived} />}
    </div>
  </div>;
}
