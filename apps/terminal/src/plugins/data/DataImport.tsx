import { ImportIdentityPicker } from "./ImportIdentity";
import { TimePicker, type TimeVersion } from "../trading_time/public";
import type { RequestClient } from "../../api/requests";
import { useRef, useState } from "react";

import type { components } from "../../api/schema";
import type { Job } from "../tasks/public";
type Options = components["schemas"]["ImportOptions"];
type Preview = components["schemas"]["ImportPreview"];
const barFields =
  "contract,event_time,available_at,trading_day,open,high,low,close,volume";
const dailyFields = "contract,trading_day,open,high,low,close,vol,amount,oi,settle";
export function DataImport({
  api,
  connected,
  onSubmitted,
}: {
  api: RequestClient;
  connected: boolean;
  onSubmitted: (job: Job) => void;
}) {
  const [identity, setIdentity] = useState<Options["identity"] | null>(null);
  const [tradingTime, setTradingTime] = useState<TimeVersion | null>(null);
  const [timestampSemantics, setTimestampSemantics] = useState<"bar_start" | "bar_end" | null>(null);
  const [source, setSource] = useState("");
  const [sourceId, setSourceId] = useState("local_history");
  const [type, setType] = useState<Options["type_id"]>("futures.daily");
  const [frequency, setFrequency] = useState<Options["frequency"]>("1m");
  const [mapping, setMapping] = useState<Record<string, string>>({});
  const [csv, setCsv] = useState("");
  const [name, setName] = useState("未选择文件");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [preview, setPreview] = useState<Preview>();
  const [checked, setChecked] = useState("");
  const command = useRef<{ key: string; id: string } | undefined>(undefined);
  const options = {
    identity,
    trading_time: tradingTime,
    timestamp_semantics: timestampSemantics,
    type_id: type,
    frequency: type === "futures.daily" ? "1d" : frequency,
    source_id: sourceId,
    column_mapping: mapping,
  };
  const key = JSON.stringify({ source, csv, options });
  const latest = useRef(key);
  latest.current = key;
  const fields = (type === "futures.daily" ? dailyFields : barFields).split(
    ",",
  );
  const presetKey = `asterion.import.mapping.v1.${type}`;
  async function check() {
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const result = await api.request<Preview>("/imports/preview", {
        command_id: "preview",
        source: source || "预览",
        csv,
        options,
      });
      if (latest.current === key) {
        setPreview(result);
        setChecked(key);
      }
    } catch (e) {
      setError(String(e));
      setChecked("");
    } finally {
      setBusy(false);
    }
  }
  async function submit() {
    setBusy(true);
    setError("");
    try {
      if (command.current?.key !== key)
        command.current = { key, id: crypto.randomUUID() };
      const job = await api.request<Job>("/imports", {
        command_id: command.current.id,
        source,
        csv,
        options,
      });
      onSubmitted(job);
      setNotice(`请求已接收 · ${job.id}`);
      setChecked("");
      command.current = undefined;
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  function template() {
    const text =
      type === "futures.daily"
        ? dailyFields + "\nSHFE.rb2610,2026-09-15,3200,3220,3190,3210,100,,,3205\n"
        : barFields +
          "\nSHFE.rb2610,2026-09-15T09:01:00+08:00,2026-09-15T09:01:01+08:00,2026-09-15,3200,3220,3190,3210,100\n";
    const url = URL.createObjectURL(
      new Blob(["\ufeff" + text], { type: "text/csv;charset=utf-8" }),
    );
    const link = document.createElement("a");
    link.href = url;
    link.download = `${type}-example.csv`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  return (
    <section className="import-panel">
      <div className="panel-heading">
        <h2>导入历史行情</h2>
        <span className="panel-spacer" />
        <small>UTF-8 CSV · 最大 2 MB</small>
      </div>
      <ImportIdentityPicker api={api} disabled={!connected || busy} onChange={setIdentity} />
      <TimePicker api={api} value={tradingTime} onChange={setTradingTime} disabled={!connected || busy} />
      {type === "futures.bars" && <label>时间戳口径<select aria-label="时间戳口径" value={timestampSemantics ?? ""} onChange={e => setTimestampSemantics(e.target.value as "bar_start" | "bar_end")}><option value="">请选择</option><option value="bar_start">K 线开始时间</option><option value="bar_end">K 线结束时间</option></select></label>}
      <fieldset disabled={busy} className="import-options">
        <label>
          数据类型
          <select
            aria-label="导入数据类型"
            value={type}
            onChange={(e) => {
              setType(e.target.value as Options["type_id"]);
              setMapping({});
              setPreview(undefined);
            }}
          >
            <option value="futures.daily">期货日线</option>
            <option value="futures.bars">带时间的 K 线</option>
          </select>
        </label>
        {type === "futures.bars" && (
          <label>
            频率
            <select
              aria-label="导入频率"
              value={frequency}
              onChange={(e) =>
                setFrequency(e.target.value as Options["frequency"])
              }
            >
              {["1m", "5m", "15m", "30m", "1h", "unspecified"].map((f) => (
                <option key={f}>{f}</option>
              ))}
            </select>
          </label>
        )}
        <label>
          来源标识
          <input
            aria-label="来源标识"
            value={sourceId}
            onChange={(e) => setSourceId(e.target.value)}
            placeholder="vendor_history"
          />
        </label>
        <label>
          来源说明
          <input
            aria-label="来源说明"
            value={source}
            onChange={(e) => setSource(e.target.value)}
            placeholder="供应商或自有采集说明"
          />
        </label>
        <button onClick={template}>下载示例模板</button>
        <label className="file-button">
          选择 CSV
          <input
            type="file"
            accept=".csv,text/csv"
            onChange={async (e) => {
              const file = e.target.files?.[0];
              if (!file) return;
              if (file.size > 2_000_000) {
                setError("CSV 超过 2 MB");
                return;
              }
              setName(file.name);
              setCsv(await file.text());
              setPreview(undefined);
              setChecked("");
            }}
          />
        </label>
        <span>{name}</span>
      </fieldset>
      <p className="settings-note">
        来源标识用于归组版本，使用小写字母、数字和下划线。同一来源持续使用相同标识。价格按元、成交量按手；日线成交额按万元。
      </p>
      <p className="settings-note">
        {type === "futures.daily"
          ? "日线文件按单个标准合约导入；交易日必须明确。导入时间仅表示本次可获知时间，不证明历史上已经可用。"
          : "时间字段必须携带时区。交易日列可省略，由时间版本推导；提供时必须一致。available_at 表示实际可获知时间，不可早于 event_time。"}
      </p>
      <textarea
        className="csv-editor"
        aria-label="CSV 数据"
        disabled={busy}
        spellCheck={false}
        value={csv}
        onChange={(e) => {
          setCsv(e.target.value);
          setPreview(undefined);
        }}
        placeholder={type === "futures.daily" ? dailyFields : barFields}
      />
      <div className="source-actions">
        <button
          disabled={busy || !connected || !csv || !sourceId || !identity}
          onClick={() => void check()}
        >
          识别字段并预览
        </button>
        <button
          disabled={busy}
          onClick={() => {
            try {
              localStorage.setItem(presetKey, JSON.stringify(mapping));
              setNotice("字段映射已保存到本机");
            } catch (e) {
              setError(String(e));
            }
          }}
        >
          保存字段映射
        </button>
        <button
          disabled={busy}
          onClick={() => {
            try {
              const value = JSON.parse(localStorage.getItem(presetKey) ?? "{}");
              if (
                !value ||
                typeof value !== "object" ||
                Array.isArray(value) ||
                Object.entries(value).some(
                  ([k, v]) => !fields.includes(k) || typeof v !== "string",
                )
              )
                throw new Error("映射格式无效");
              setMapping(value);
              setChecked("");
            } catch (e) {
              setError(String(e));
            }
          }}
        >
          载入字段映射
        </button>
        <button
          className="primary"
          disabled={
            busy ||
            !connected ||
            !source.trim() ||
            !preview?.valid ||
            checked !== key
          }
          onClick={() => void submit()}
        >
          创建采集任务
        </button>
      </div>
      {preview && (
        <>
          <div className="import-options">
            {fields.map((field) => (
              <label key={field}>
                {field}
                {preview.required.includes(field) ? " *" : ""}
                <select
                  aria-label={`映射 ${field}`}
                  disabled={busy}
                  value={
                    mapping[field] ??
                    (preview.columns.includes(field) &&
                    !Object.keys(mapping).length
                      ? field
                      : "")
                  }
                  onChange={(e) => {
                    const next = Object.keys(mapping).length
                      ? { ...mapping }
                      : Object.fromEntries(
                          fields
                            .filter((f) => preview.columns.includes(f))
                            .map((f) => [f, f]),
                        );
                    if (e.target.value) next[field] = e.target.value;
                    else delete next[field];
                    setMapping(next);
                  }}
                >
                  <option value="">未映射</option>
                  {preview.columns.map((col) => (
                    <option key={col}>{col}</option>
                  ))}
                </select>
              </label>
            ))}
          </div>
          {checked !== key && (
            <p className="settings-note">输入已变化，请重新预览后提交。</p>
          )}
          {preview.errors.map((message) => (
            <div className="alert" key={message}>
              {message}
            </div>
          ))}
          {checked === key && preview.valid && (
            <>
              <p>校验通过 · 共 {preview.total} 行 · 预览前 10 行</p>
              <p aria-label="导入实际合约">实际合约：{preview.contract_ids.join("、")}</p>
              <div className="import-preview">
                <table className="data-table">
                  <thead>
                    <tr>
                      {fields.map((f) => (
                        <th key={f}>{f}</th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {preview.rows.map((row, i) => (
                      <tr key={i}>
                        {fields.map((f) => (
                          <td key={f}>{String(row[f] ?? "—")}</td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </>
          )}
        </>
      )}
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
