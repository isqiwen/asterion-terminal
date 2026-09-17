import { useState } from "react";
import type { Job } from "../api/client";
import { request } from "../api/client";
export function DataImport({
  token,
  connected,
  onSubmitted,
}: {
  token: string;
  connected: boolean;
  onSubmitted: (job: Job) => void;
}) {
  const [source, setSource] = useState("");
  const [csv, setCsv] = useState("");
  const [name, setName] = useState("未选择文件");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  async function submit() {
    setBusy(true);
    setError("");
    try {
      const job = await request<Job>("/imports", token, {
        command_id: crypto.randomUUID(),
        source,
        csv,
      });
      onSubmitted(job);
      setNotice(`请求已接收 · ${job.id}`);
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <section className="import-panel">
      <div className="panel-heading">
        <h2>导入历史行情</h2>
        <span className="panel-spacer" />
        <small>CSV · 最大 2 MB</small>
      </div>
      <div className="import-toolbar">
        <label className="file-button">
          选择 CSV
          <input
            type="file"
            accept=".csv,text/csv"
            onChange={async (e) => {
              const f = e.target.files?.[0];
              if (!f) return;
              if (f.size > 2_000_000) {
                setError("CSV 超过 2 MB");
                return;
              }
              setName(f.name);
              setCsv(await f.text());
            }}
          />
        </label>
        <span className="file-name">{name}</span>
        <input
          aria-label="来源说明"
          placeholder="供应商 / 文件来源 / 数据频率"
          value={source}
          onChange={(e) => setSource(e.target.value)}
        />
        <button
          className="primary"
          disabled={!connected || busy || !source || !csv}
          onClick={submit}
        >
          {busy ? "提交中…" : "创建采集任务"}
        </button>
      </div>
      <textarea
        className="csv-editor"
        aria-label="CSV 数据"
        spellCheck={false}
        value={csv}
        onChange={(e) => setCsv(e.target.value)}
        placeholder="contract,event_time,available_at,trading_day,open,high,low,close,volume"
      />
      <div className="editor-status">
        <span>UTF-8</span>
        <span>{csv ? csv.split("\n").length : 0} 行</span>
        <span className="panel-spacer" />
        <span>交易日须显式提供</span>
      </div>
      {error && (
        <div role="alert" className="alert">
          {error}
        </div>
      )}
      {notice && (
        <div role="status" className="notice">
          {notice}
        </div>
      )}
    </section>
  );
}
