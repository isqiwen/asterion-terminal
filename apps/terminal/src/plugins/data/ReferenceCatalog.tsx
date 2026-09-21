import type { RequestClient } from "../../api/requests";
import { useEffect, useRef, useState } from "react";

import type { components } from "../../api/schema";
import { SourceCatalog } from "./SourceCatalog";
type Summary = components["schemas"]["ReferenceSummary"];
type Release = components["schemas"]["ReferenceRelease"];
export function ReferenceCatalog({
  api,
  connected,
}: {
  api: RequestClient;
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
    api
      .request<Summary[]>(`/reference/releases?limit=20&offset=${offset}`)
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
  }, [api, connected, offset, revision]);
  async function open(id: string) {
    const serial = ++selection.current;
    try {
      const value = await api.request<Release>(`/reference/releases/${id}`);
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
      const value = await api.request<Release>("/reference/releases", catalog);
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
        <h2>合约目录版本</h2>
        <span className="panel-spacer" />
        <label className="reference-upload">
          {busy ? "正在发布…" : "导入目录文件"}
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
        <SourceCatalog
          api={api}
          connected={connected}
          busy={busy}
          setBusy={setBusy}
          onPublished={(value) => {
            setError("");
            selection.current++;
            setRelease(value);
            setOffset(0);
            refresh((n) => n + 1);
          }}
        />
        <p className="panel-footnote">
          目录按版本保存。来源由提供方声明，导入不会自动认定为交易所权威数据。
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
              ? "尚无合约目录版本。可在此导入品种与实际合约目录。"
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
                  <th>上市 / 最后交易日</th>
                  <th>最后交割日</th>
                  <th>来源 / 版本</th>
                </tr>
              </thead>
              <tbody>
                {release.catalog.contracts.map((c) => (
                  <tr key={c.id}>
                    <td>{c.id}</td>
                    <td>{c.product_id}</td>
                    <td>{c.delivery_month}</td>
                    <td>
                      {c.listed_on} — {c.last_trade_on}
                    </td>
                    <td>{c.last_delivery_on ?? "未提供"}</td>
                    <td>
                      {c.provenance.source} / {c.provenance.source_version}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
            {release.catalog.inputs.length > 0 && (
              <div aria-label="目录输入证据">
                <h3>固定资料依据</h3>
                {release.catalog.inputs.map((input) => (
                  <p key={input.version_id}>
                    {input.source} · {input.version_id}
                    <br />
                    SHA-256：{input.checksum}
                  </p>
                ))}
              </div>
            )}
            <div className="panel-heading">
              <h2>来源代码映射</h2>
            </div>
            <table className="data-table">
              <thead>
                <tr>
                  <th>来源</th>
                  <th>代码</th>
                  <th>实际合约身份</th>
                  <th>有效交易日</th>
                  <th>信息可用时间</th>
                </tr>
              </thead>
              <tbody>
                {release.catalog.symbols.map((m) => (
                  <tr key={`${m.source}:${m.symbol}:${m.valid_from}`}>
                    <td>{m.source}</td>
                    <td>{m.symbol}</td>
                    <td>{m.contract_id}</td>
                    <td>
                      {m.valid_from} — {m.valid_until}
                    </td>
                    <td>{m.provenance.available_at}</td>
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
