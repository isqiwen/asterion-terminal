import { useEffect, useRef, useState } from "react";
import { request } from "../api/client";
import type { components } from "../api/schema";
type Summary = components["schemas"]["ReferenceSummary"];
type Release = components["schemas"]["ReferenceRelease"];
export function ReferenceCatalog({
  token,
  connected,
}: {
  token: string;
  connected: boolean;
}) {
  const [rows, setRows] = useState<Summary[]>([]);
  const [release, setRelease] = useState<Release>();
  const [offset, setOffset] = useState(0);
  const [revision, refresh] = useState(0);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const selection = useRef(0);
  useEffect(() => {
    let active = true;
    if (!connected) return;
    request<Summary[]>(`/reference/releases?limit=20&offset=${offset}`, token)
      .then((value) => {
        if (active) {
          setRows(value);
          setError("");
        }
      })
      .catch((e) => {
        if (active) setError(String(e));
      });
    return () => {
      active = false;
    };
  }, [token, connected, offset, revision]);
  async function open(id: string) {
    const serial = ++selection.current;
    try {
      const value = await request<Release>(`/reference/releases/${id}`, token);
      if (selection.current === serial) {
        setRelease(value);
        setError("");
      }
    } catch (e) {
      if (selection.current === serial) setError(String(e));
    }
  }
  async function upload(file: File) {
    setBusy(true);
    setError("");
    try {
      if (file.size > 4_000_000) throw new Error("目录文件不能超过 4 MB");
      const catalog = JSON.parse(await file.text());
      const value = await request<Release>(
        "/reference/releases",
        token,
        catalog,
      );
      selection.current++;
      setRelease(value);
      setOffset(0);
      refresh((n) => n + 1);
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <div className="panel-heading">
        <h2>合约规则版本</h2>
        <span className="panel-spacer" />
        <label className="reference-upload">
          {busy ? "正在发布…" : "导入规则文件"}
          <input
            aria-label="导入合约资料"
            type="file"
            accept=".json,application/json"
            disabled={!connected || busy}
            onChange={(e) => {
              const file = e.target.files?.[0];
              if (file) void upload(file);
              e.target.value = "";
            }}
          />
        </label>
      </div>
      {error && (
        <div className="notice" role="alert">
          {error}
        </div>
      )}
      <div className="panel-scroll">
        <p className="panel-footnote">
          规则按版本保存。来源由提供方声明，导入不会自动认定为交易所权威数据。
        </p>
        <table className="data-table">
          <thead>
            <tr>
              <th>版本</th>
              <th>发布时间</th>
              <th>来源</th>
              <th>品种</th>
              <th>合约</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((row) => (
              <tr
                key={row.id}
                className={release?.id === row.id ? "selected" : ""}
              >
                <td>
                  <button onClick={() => void open(row.id)}>
                    {row.id.slice(0, 12)}
                  </button>
                </td>
                <td>{new Date(row.published_at * 1000).toLocaleString()}</td>
                <td>{row.sources.join(" · ")}</td>
                <td>{row.products}</td>
                <td>{row.contracts}</td>
              </tr>
            ))}
          </tbody>
        </table>
        {!rows.length && (
          <div className="table-empty-state">
            {connected
              ? "尚无合约规则版本。基础资料由数据源同步；完整规则文件可在此导入。"
              : "连接本机服务后查看目录"}
          </div>
        )}
        <div className="panel-heading">
          <button
            disabled={offset === 0}
            onClick={() => setOffset((n) => Math.max(0, n - 20))}
          >
            上一页
          </button>
          <span>第 {Math.floor(offset / 20) + 1} 页</span>
          <button
            disabled={rows.length < 20}
            onClick={() => setOffset((n) => n + 20)}
          >
            下一页
          </button>
        </div>
        {release && (
          <>
            <div className="panel-heading">
              <h2>合约明细</h2>
              <small title={release.id}>{release.id.slice(0, 12)}</small>
            </div>
            <table className="data-table">
              <thead>
                <tr>
                  <th>合约</th>
                  <th>品种</th>
                  <th>交割月</th>
                  <th>乘数</th>
                  <th>最小变动价位</th>
                  <th>来源 / 版本</th>
                </tr>
              </thead>
              <tbody>
                {release.catalog.contracts.map((c) => (
                  <tr key={c.id}>
                    <td>{c.id}</td>
                    <td>{c.product_id}</td>
                    <td>{c.delivery_month}</td>
                    <td>{c.multiplier}</td>
                    <td>{c.tick_size}</td>
                    <td>
                      {c.provenance.source} / {c.provenance.source_version}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
            <div className="panel-heading">
              <h2>交易规则</h2>
              <small>{release.catalog.calendars.length} 个交易日历版本</small>
            </div>
            <table className="data-table">
              <thead>
                <tr>
                  <th>合约 / 版本</th>
                  <th>生效时间</th>
                  <th>保证金比例</th>
                  <th>平今区分</th>
                </tr>
              </thead>
              <tbody>
                {release.catalog.rules.map((r) => (
                  <tr key={`${r.contract_id}-${r.version}`}>
                    <td>
                      {r.contract_id} / {r.version}
                    </td>
                    <td>
                      {r.effective_from} — {r.effective_until ?? "持续有效"}
                    </td>
                    <td>{r.margin_rate}</td>
                    <td>{r.requires_close_today ? "是" : "否"}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </>
        )}
      </div>
    </>
  );
}
