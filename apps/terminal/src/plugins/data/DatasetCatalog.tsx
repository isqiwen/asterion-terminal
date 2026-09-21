import type { RequestClient } from "../../api/requests";
import { useEffect, useRef, useState } from "react";

import type { Job } from "../tasks/public";
import { type Snapshot } from "./client";
import { DailyCoveragePanel } from "./DailyCoveragePanel";
import { IngestionDetails } from "./IngestionDetails";
import { VersionLifecycle } from "./VersionLifecycle";

type DataType = {
  id: string;
  label: string;
  domain: string;
  domain_label: string;
  shape: string;
  frequency: string;
  schema_version: number;
  primary_key: string[];
  time_semantics: string;
  description: string;
  fields: { name: string; label: string; unit: string | null }[];
};
type Version = {
  archived?: boolean;
  id: string;
  dataset_id: string;
  job_id: string;
  created_at: number;
  rows: number;
  version_count?: number;
  manifest: {
    type: DataType;
    source: string;
    origin?: {
      name?: string;
      source_id?: string;
      connection_id?: string;
      method?: string;
    };
    demo?: boolean;
    layer: string;
    scope: Record<string, unknown>;
    format: string;
    checksum: string;
    bytes: number;
    inputs: string[];
    first: string | null;
    last: string | null;
    coverage: string;
    quality: string;
    version_semantics?: string;
    parent_version_id?: string | null;
    revision?: number;
    acquired_rows?: number;
    logical_bytes?: number;
    changes?: {
      added: number;
      revised: number;
      refreshed: number;
      unchanged: number;
      stale_ignored: number;
    };
    partitions?: {
      key: string;
      rows: number;
      first: string;
      last: string;
      checksum: string;
    }[];
    coverage_gaps?: { start: string; end: string }[] | null;
    transform: { id: string; version: string } | null;
  };
};
type Page = { items: Version[]; total: number; offset: number };
type Preview = {
  version: Version;
  rows: Record<string, unknown>[];
  total: number;
  offset: number;
  snapshot: Snapshot | null;
  row_sources?: { observed_at: string; raw_version_id: string }[] | null;
};
const layers: Record<string, string> = {
  RAW: "原始",
  STANDARD: "标准化",
  DERIVED: "衍生",
};
const shapes: Record<string, string> = {
  table: "表格",
  timeseries: "时间序列",
  document: "文档",
  graph: "关系图",
  artifact: "产物",
};
const frequencies: Record<string, string> = {
  "1d": "日频",
  snapshot: "资料快照",
  unspecified: "未声明频率",
};
const scopeLabel = (v: Version) =>
  Object.values(v.manifest.scope).filter(Boolean).map(String).join(" · ");
const coverage = (v: Version) =>
  v.manifest.coverage === "CALENDAR_COMPLETE"
    ? "日历日期完整"
    : v.manifest.coverage === "CALENDAR_GAPS"
      ? "日历存在缺口"
      : v.manifest.coverage === "IMPORTED_ROWS_ONLY"
        ? "仅导入记录"
        : "仅已返回记录";

export function DatasetCatalog({
  api,
  connected,
  onSnapshot,
  onSync,
  contractsOnly = false,
  onRefill,
  initialVersionId,
  initialReportId,
}: {
  api: RequestClient;
  connected: boolean;
  onSnapshot: (snapshot: Snapshot) => void;
  onSync: () => void;
  contractsOnly?: boolean;
  onRefill?: (jobs: Job[], reportId?: string) => void;
  initialVersionId?: string;
  initialReportId?: string;
}) {
  const [includeArchived, setIncludeArchived] = useState(false);
  const [types, setTypes] = useState<DataType[]>([]);
  const [sources, setSources] = useState<{ id: string; name: string }[]>([]);
  const [domain, setDomain] = useState("");
  const [typeId, setTypeId] = useState("");
  const [source, setSource] = useState("");
  const [ingestionVersion, setIngestionVersion] = useState<string | null>(null);
  const [layer, setLayer] = useState("STANDARD");
  const [search, setSearch] = useState("");
  const [offset, setOffset] = useState(0);
  const [catalog, setCatalog] = useState<Page>({
    items: [],
    total: 0,
    offset: 0,
  });
  const [history, setHistory] = useState<Page>({
    items: [],
    total: 0,
    offset: 0,
  });
  const [preview, setPreview] = useState<Preview>();
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const selection = useRef(0);
  useEffect(() => {
    if (!connected) return;
    let active = true;
    Promise.all([
      api.request<DataType[]>("/data/types"),
      api.request<{ id: string; name: string }[]>("/data/providers"),
    ])
      .then(([definitions, providers]) => {
        if (active) {
          setTypes(definitions);
          setSources(providers);
        }
      })
      .catch((e) => {
        if (active) setError(String(e));
      });
    return () => {
      active = false;
    };
  }, [api, connected]);
  useEffect(() => {
    selection.current++;
    setBusy(false);
    setPreview(undefined);
    setHistory({ items: [], total: 0, offset: 0 });
    if (!connected) return;
    let active = true;
    const query = new URLSearchParams({
      domain,
      type_id: contractsOnly ? "futures.contracts" : typeId,
      source,
      layer,
      search,
      offset: String(offset),
      include_archived: String(includeArchived),
    });
    const refresh = () =>
      api
        .request<Page>(`/data/catalog?${query}`)
        .then((result) => {
          if (active) {
            setCatalog(result);
            setError("");
          }
        })
        .catch((e) => {
          if (active) setError(String(e));
        });
    setCatalog({ items: [], total: 0, offset });
    void refresh();
    const timer = setInterval(() => void refresh(), 3000);
    window.addEventListener("asterion:unlocked", refresh);
    window.addEventListener("asterion:catalog-changed", refresh);
    return () => {
      active = false;
      clearInterval(timer);
      window.removeEventListener("asterion:unlocked", refresh);
      window.removeEventListener("asterion:catalog-changed", refresh);
    };
  }, [
    api,
    connected,
    domain,
    typeId,
    source,
    layer,
    search,
    offset,
    contractsOnly,
    includeArchived,
  ]);
  useEffect(() => {
    if (connected && initialVersionId) void inspect(initialVersionId);
    return () => {
      selection.current++;
    };
  }, [connected, api, initialVersionId]);
  async function inspect(id: string, rowOffset = 0) {
    const sequence = ++selection.current;
    setBusy(true);
    setError("");
    try {
      const result = await api.request<Preview>(
        `/data/versions/${id}?offset=${rowOffset}`,
      );
      const versions = await api.request<Page>(
        `/data/catalog/${result.version.dataset_id}/versions`,
      );
      if (sequence === selection.current) {
        setPreview(result);
        setHistory(versions);
      }
    } catch (e) {
      if (sequence === selection.current) setError(String(e));
    } finally {
      if (sequence === selection.current) setBusy(false);
    }
  }
  async function historyPage(next: number) {
    if (!preview) return;
    const sequence = selection.current;
    try {
      const result = await api.request<Page>(
        `/data/catalog/${preview.version.dataset_id}/versions?offset=${next}`,
      );
      if (sequence === selection.current) setHistory(result);
    } catch (e) {
      if (sequence === selection.current) setError(String(e));
    }
  }
  const definition = preview?.version.manifest.type;
  const domains = [
    ...new Map(types.map((t) => [t.domain, t.domain_label])).entries(),
  ];
  const fields = new Map(
    definition?.fields.map((f) => [
      f.name,
      f.unit ? `${f.label} / ${f.unit}` : f.label,
    ]),
  );
  const filter = (update: () => void) => {
    update();
    setOffset(0);
  };
  return (
    <>
      <div className="panel-heading">
        <h2>{contractsOnly ? "合约资料" : "数据集"}</h2>
        <span className="count">{catalog.total}</span>
        <span className="panel-spacer" />
        <small>按数据集组织 · 保留历史版本</small>
        <button onClick={onSync}>
          {contractsOnly ? "同步合约资料" : "获取数据"}
        </button>
      </div>
      <div className="sync-form catalog-filters">
        <label>
          <input
            type="checkbox"
            checked={includeArchived}
            onChange={(e) => filter(() => setIncludeArchived(e.target.checked))}
          />
          显示归档版本
        </label>

        {!contractsOnly && (
          <>
            <label>
              业务类别
              <select
                aria-label="业务类别"
                value={domain}
                onChange={(e) =>
                  filter(() => {
                    setDomain(e.target.value);
                    setTypeId("");
                  })
                }
              >
                <option value="">全部类别</option>
                {domains.map(([id, name]) => (
                  <option key={id} value={id}>
                    {name}
                  </option>
                ))}
              </select>
            </label>
            <label>
              数据类型
              <select
                aria-label="目录数据类型"
                value={typeId}
                onChange={(e) => filter(() => setTypeId(e.target.value))}
              >
                <option value="">全部类型</option>
                {types
                  .filter((t) => !domain || t.domain === domain)
                  .map((t) => (
                    <option key={t.id} value={t.id}>
                      {t.label}
                    </option>
                  ))}
              </select>
            </label>
          </>
        )}
        <label>
          来源
          <select
            aria-label="目录来源"
            value={source}
            onChange={(e) => filter(() => setSource(e.target.value))}
          >
            <option value="">全部来源</option>
            {sources.map((s) => (
              <option key={s.id} value={s.id}>
                {s.name}
              </option>
            ))}
            <option value="local_file">本地文件</option>
          </select>
        </label>
        <label>
          加工阶段
          <select
            aria-label="加工阶段"
            value={layer}
            onChange={(e) => filter(() => setLayer(e.target.value))}
          >
            <option value="">全部阶段</option>
            {Object.entries(layers).map(([id, name]) => (
              <option key={id} value={id}>
                {name}
              </option>
            ))}
          </select>
        </label>
        <label>
          搜索
          <input
            aria-label="搜索数据集"
            placeholder="合约、交易所或导入名称"
            value={search}
            onChange={(e) => filter(() => setSearch(e.target.value))}
          />
        </label>
      </div>
      {error && (
        <div className="alert" role="alert">
          {error}
        </div>
      )}
      <div className="panel-scroll sync-releases">
        <table className="data-table">
          <thead>
            <tr>
              <th>类型</th>
              <th>类别</th>
              <th>来源</th>
              <th>加工阶段</th>
              <th>对象</th>
              <th>频率</th>
              <th>最新版本范围</th>
              <th>行数</th>
              <th>覆盖说明</th>
              <th>版本语义</th>
              <th>版本数</th>
            </tr>
          </thead>
          <tbody>
            {catalog.items.map((v) => (
              <tr
                key={v.dataset_id}
                className={
                  preview?.version.dataset_id === v.dataset_id ? "selected" : ""
                }
              >
                <td>
                  <button onClick={() => void inspect(v.id)}>
                    {v.manifest.type.label}
                  </button>
                  {v.archived && <small> · 已归档</small>}
                </td>
                <td>{v.manifest.type.domain_label}</td>
                <td title={v.manifest.source}>
                  {v.manifest.demo === true
                    ? `${v.manifest.origin?.name ?? "合成示例"} · 非真实行情`
                    : (v.manifest.origin?.name ?? v.manifest.source)}
                </td>
                <td>{layers[v.manifest.layer]}</td>
                <td>{scopeLabel(v)}</td>
                <td>
                  {frequencies[v.manifest.type.frequency] ??
                    v.manifest.type.frequency}
                </td>
                <td>
                  {v.manifest.first
                    ? `${v.manifest.first} — ${v.manifest.last}`
                    : "资料快照"}
                </td>
                <td>{v.rows}</td>
                <td>{coverage(v)}</td>
                <td>
                  {v.manifest.version_semantics === "CUMULATIVE"
                    ? "累积版本"
                    : "采集范围"}
                </td>
                <td>{v.version_count}</td>
              </tr>
            ))}
          </tbody>
        </table>
        {!catalog.items.length && (
          <div className="table-empty">
            没有符合条件的数据集。可调整筛选，或通过数据同步获取、导入数据。
          </div>
        )}
      </div>
      <div className="panel-heading">
        <small>
          日线与日历的标准版本累积保留已采集记录；其他版本保留各次采集范围。
        </small>
        <span className="panel-spacer" />
        <button disabled={!offset} onClick={() => setOffset(offset - 50)}>
          上一页数据集
        </button>
        <span>
          {catalog.total ? offset + 1 : 0}–
          {Math.min(offset + 50, catalog.total)} / {catalog.total}
        </span>
        <button
          disabled={offset + 50 >= catalog.total}
          onClick={() => setOffset(offset + 50)}
        >
          下一页数据集
        </button>
      </div>
      {busy && (
        <div className="panel-footnote" role="status">
          正在读取数据版本…
        </div>
      )}
      {preview && (
        <>
          <div className="panel-heading">
            <h2>
              {definition?.label} · {layers[preview.version.manifest.layer]}预览
              {preview.version.manifest.demo === true &&
                " · 合成示例 · 非真实行情"}
            </h2>
            <span className="panel-spacer" />
            {preview.version.manifest.source !== "local_file" && (
              <button onClick={() => setIngestionVersion(preview.version.id)}>
                查看采集记录
              </button>
            )}
            {preview.snapshot && (
              <button onClick={() => onSnapshot(preview.snapshot!)}>
                查看历史图表
              </button>
            )}
            <button
              disabled={preview.offset === 0 || busy}
              onClick={() =>
                void inspect(preview.version.id, preview.offset - 100)
              }
            >
              上一页
            </button>
            <span>
              {preview.total ? preview.offset + 1 : 0}–
              {Math.min(preview.offset + 100, preview.total)} / {preview.total}
            </span>
            <button
              disabled={preview.offset + 100 >= preview.total || busy}
              onClick={() =>
                void inspect(preview.version.id, preview.offset + 100)
              }
            >
              下一页
            </button>
          </div>
          <VersionLifecycle
            key={`${preview.version.id}:${api.key}`}
            versionId={preview.version.id}
            api={api}
            disabled={!connected || busy}
          />
          <div className="catalog-version-bar">
            <label>
              采集版本
              <select
                aria-label="采集版本"
                value={preview.version.id}
                onChange={(e) => void inspect(e.target.value)}
              >
                {!history.items.some((v) => v.id === preview.version.id) && (
                  <option value={preview.version.id}>
                    {preview.version.id.slice(0, 8)} · 当前查看
                  </option>
                )}
                {history.items.map((v) => (
                  <option key={v.id} value={v.id}>
                    {new Date(v.created_at * 1000).toLocaleString("zh-CN")} ·{" "}
                    {v.id.slice(0, 8)} · {v.rows} 行{" "}
                    {v.archived ? "· 已归档" : ""}
                  </option>
                ))}
              </select>
            </label>
            <button
              disabled={!history.offset}
              onClick={() => void historyPage(history.offset - 50)}
            >
              较新版本
            </button>
            <span>{history.total} 个版本</span>
            <button
              disabled={history.offset + 50 >= history.total}
              onClick={() => void historyPage(history.offset + 50)}
            >
              较早版本
            </button>
            {preview.version.manifest.inputs.map((id) => (
              <button key={id} onClick={() => void inspect(id)}>
                {id === preview.version.manifest.parent_version_id
                  ? "查看父版本"
                  : "查看原始输入"}
              </button>
            ))}
          </div>
          {ingestionVersion === preview.version.id && (
            <IngestionDetails
              key={preview.version.id}
              job={{
                id: preview.version.job_id,
                state: "SUCCEEDED",
                error: null,
              }}
              api={api}
              onBack={() => setIngestionVersion(null)}
            />
          )}
          <dl className="dataset-summary" aria-label="数据版本摘要">
            <div>
              <dt>来源连接</dt>
              <dd>
                {preview.version.manifest.origin?.name ??
                  preview.version.manifest.source}
              </dd>
            </div>
            <div>
              <dt>频率</dt>
              <dd>
                {frequencies[preview.version.manifest.type.frequency] ??
                  preview.version.manifest.type.frequency}
              </dd>
            </div>
            <div>
              <dt>数据范围</dt>
              <dd>
                {scopeLabel(preview.version)} ·{" "}
                {preview.version.manifest.first ?? "未知"} —{" "}
                {preview.version.manifest.last ?? "未知"}
              </dd>
            </div>
            <div>
              <dt>覆盖情况</dt>
              <dd>{coverage(preview.version)}；未核对不代表没有缺失</dd>
            </div>
            <div>
              <dt>校验状态</dt>
              <dd>
                {preview.version.manifest.quality === "VALIDATED"
                  ? "已通过结构和数值校验；不等于数据完整或适合实盘"
                  : "原始采集记录，尚未标准化"}
              </dd>
            </div>
            <div>
              <dt>固定版本</dt>
              <dd>{preview.version.id}</dd>
            </div>
            <div>
              <dt>采集任务</dt>
              <dd>
                {preview.version.job_id} ·{" "}
                {new Date(preview.version.created_at * 1000).toLocaleString()}
              </dd>
            </div>
          </dl>
          {preview.version.manifest.origin && (
            <p className="settings-note">
              获取方式：
              {preview.version.manifest.origin.method === "file"
                ? "文件导入"
                : "API 同步"}{" "}
              · 来源：{preview.version.manifest.origin.name} · 标识：
              {preview.version.manifest.origin.source_id ??
                preview.version.manifest.origin.connection_id ??
                preview.version.manifest.source}
            </p>
          )}
          {definition?.id === "futures.daily" &&
            preview.version.manifest.layer === "STANDARD" &&
            (preview.version.manifest.version_semantics === "CUMULATIVE" ||
              preview.version.manifest.source === "local_file") && (
              <DailyCoveragePanel
                local={preview.version.manifest.source === "local_file"}
                key={preview.version.id}
                initialReportId={
                  preview.version.id === initialVersionId
                    ? initialReportId
                    : undefined
                }
                versionId={preview.version.id}
                api={api}
                first={preview.version.manifest.first!}
                last={preview.version.manifest.last!}
                onInspect={(id) => void inspect(id)}
                onRefill={onRefill}
              />
            )}
          {preview.version.manifest.version_semantics === "CUMULATIVE" && (
            <div
              className="catalog-cumulative panel-scroll"
              aria-label="累积版本详情"
            >
              <p>
                累积版本 #{preview.version.manifest.revision} · 本次采集{" "}
                {preview.version.manifest.acquired_rows} 行 · 累积{" "}
                {preview.total} 行
              </p>
              {preview.version.manifest.changes && (
                <p>
                  新增 {preview.version.manifest.changes.added} 行 · 修订{" "}
                  {preview.version.manifest.changes.revised} 行 · 同值刷新{" "}
                  {preview.version.manifest.changes.refreshed} 行 · 未变化{" "}
                  {preview.version.manifest.changes.unchanged} 行 · 忽略较旧响应{" "}
                  {preview.version.manifest.changes.stale_ignored} 行
                </p>
              )}
              <p>
                重叠记录采用较新的采集结果；缺失行不作删除，旧版本保持不变。引用分区{" "}
                {(
                  Number(preview.version.manifest.logical_bytes ?? 0) / 1024
                ).toFixed(1)}{" "}
                KB（跨版本共享，不等于新增磁盘占用）。
              </p>
              {!!preview.version.manifest.coverage_gaps?.length && (
                <p role="status">
                  未覆盖的日历日期：
                  {preview.version.manifest.coverage_gaps
                    .map((gap) => `${gap.start} — ${gap.end}`)
                    .join("；")}
                </p>
              )}
              <table className="data-table" aria-label="版本分区">
                <thead>
                  <tr>
                    <th>月份</th>
                    <th>已返回日期范围</th>
                    <th>行数</th>
                    <th>分区校验和</th>
                  </tr>
                </thead>
                <tbody>
                  {preview.version.manifest.partitions?.map((part) => (
                    <tr key={part.key}>
                      <td>{part.key}</td>
                      <td>
                        {part.first} — {part.last}
                      </td>
                      <td>{part.rows}</td>
                      <td className="mono" title={part.checksum}>
                        {part.checksum.slice(0, 16)}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
          <div className="panel-footnote">
            {shapes[definition?.shape ?? ""]} ·{" "}
            {preview.version.manifest.format} ·{" "}
            {(preview.version.manifest.bytes / 1024).toFixed(1)} KB ·{" "}
            {preview.version.manifest.layer === "RAW"
              ? "采集原始字段"
              : `主键：${definition?.primary_key.join(" + ")}`}
            <br />
            {definition?.time_semantics}
            <br />
            版本 {preview.version.id} · SHA-256{" "}
            {preview.version.manifest.checksum}
            {preview.version.manifest.transform && (
              <>
                <br />
                处理：{preview.version.manifest.transform.id} /{" "}
                {preview.version.manifest.transform.version} · 类型结构 v
                {definition?.schema_version}
              </>
            )}
          </div>
          <div className="panel-scroll">
            <table className="data-table">
              <thead>
                <tr>
                  {Object.keys(preview.rows[0] ?? {}).map((k) => (
                    <th key={k}>
                      {preview.version.manifest.layer === "RAW"
                        ? k
                        : (fields.get(k) ?? k)}
                    </th>
                  ))}
                  {preview.row_sources && (
                    <>
                      <th>采集时间</th>
                      <th>原始来源</th>
                    </>
                  )}
                </tr>
              </thead>
              <tbody>
                {preview.rows.map((row, i) => (
                  <tr key={i}>
                    {Object.entries(row).map(([k, v]) => (
                      <td className="mono" key={k}>
                        {v == null
                          ? "—"
                          : v === "INCOMPLETE"
                            ? "待补充"
                            : String(v)}
                      </td>
                    ))}
                    {preview.row_sources && (
                      <>
                        <td className="mono">
                          {preview.row_sources[i]?.observed_at}
                        </td>
                        <td>
                          <button
                            onClick={() =>
                              void inspect(
                                preview.row_sources![i].raw_version_id,
                              )
                            }
                          >
                            查看该行来源
                          </button>
                        </td>
                      </>
                    )}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="panel-footnote">
            {definition?.description} · {coverage(preview.version)}
            。字段校验不代表区间无缺失。
          </div>
        </>
      )}
    </>
  );
}
