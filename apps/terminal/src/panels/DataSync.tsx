import { DataImport } from "./DataImport";
import { useEffect, useState } from "react";
import { request, type Job, type Provider } from "../api/client";
import { openSettings } from "../settings/window";

const today = () =>
  new Intl.DateTimeFormat("en-CA", { timeZone: "Asia/Shanghai" }).format(
    new Date(),
  );

export function DataSync({
  token,
  connected,
  onSubmitted,
  onBrowse,
}: {
  token: string;
  connected: boolean;
  onSubmitted: (job: Job) => void;
  onBrowse: () => void;
}) {
  const [providers, setProviders] = useState<Provider[]>([]);
  const [providerId, setProviderId] = useState("tushare");
  const [dataset, setDataset] = useState("contracts");
  const [exchange, setExchange] = useState("SHFE");
  const [symbol, setSymbol] = useState("");
  const [start, setStart] = useState(`${new Date().getFullYear()}-01-01`);
  const [end, setEnd] = useState(today);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const provider = providers.find((p) => p.id === providerId);
  const capability = provider?.capabilities.find((c) => c.id === dataset);
  useEffect(() => {
    if (!connected) return;
    let active = true;
    const refresh = () =>
      request<Provider[]>("/data/providers", token)
        .then((p) => {
          if (active) setProviders(p);
        })
        .catch((e) => {
          if (active) setError(String(e));
        });
    void refresh();
    const timer = setInterval(() => void refresh(), 3000);
    window.addEventListener("asterion:unlocked", refresh);
    return () => {
      active = false;
      clearInterval(timer);
      window.removeEventListener("asterion:unlocked", refresh);
    };
  }, [token, connected]);
  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const job = await request<Job>("/data/sync", token, {
        command_id: crypto.randomUUID(),
        provider: providerId,
        dataset,
        exchange,
        symbol: capability?.symbol_required ? symbol.trim().toUpperCase() : "",
        start: capability?.date_range ? start : null,
        end: capability?.date_range ? end : null,
      });
      onSubmitted(job);
      setNotice("同步任务已提交，进度与异常会显示在任务中心。");
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <div className="panel-heading">
        <h2>数据同步</h2>
        <span className="panel-spacer" />
        <small>历史数据 · 插件驱动</small>
        <button
          onClick={() =>
            openSettings("数据源").catch((e) => setError(String(e)))
          }
        >
          数据源设置
        </button>
      </div>
      <form className="sync-form" onSubmit={submit}>
        <label>
          数据源
          <select
            aria-label="数据源"
            value={providerId}
            onChange={(e) => {
              setProviderId(e.target.value);
              if (e.target.value === "local_file") return;
              const p = providers.find((p) => p.id === e.target.value)!;
              setProviderId(p.id);
              setDataset(p.capabilities[0].id);
              setExchange(p.capabilities[0].exchanges[0]);
            }}
          >
            {!providers.length && (
              <option value="tushare" disabled>
                数据源待连接
              </option>
            )}
            <option value="local_file">本地文件 · CSV</option>
            {providers.map((p) => (
              <option key={p.id} value={p.id}>
                {p.name} · {p.configured ? "已配置" : "未配置"}
              </option>
            ))}
          </select>
        </label>
        {providerId !== "local_file" && (
          <>
            <label>
              数据类型
              <select
                aria-label="数据类型"
                value={dataset}
                onChange={(e) => {
                  setDataset(e.target.value);
                  const c = provider?.capabilities.find(
                    (c) => c.id === e.target.value,
                  );
                  if (c && !c.exchanges.includes(exchange))
                    setExchange(c.exchanges[0]);
                }}
              >
                {provider?.capabilities.map((c) => (
                  <option key={c.id} value={c.id}>
                    {c.label}
                  </option>
                ))}
              </select>
            </label>
            <label>
              交易所
              <select
                aria-label="交易所"
                value={exchange}
                onChange={(e) => setExchange(e.target.value)}
              >
                {capability?.exchanges.map((x) => (
                  <option key={x}>{x}</option>
                ))}
              </select>
            </label>
            {capability?.symbol_required && (
              <label>
                实际合约代码
                <input
                  required
                  aria-label="实际合约代码"
                  placeholder="RB2610.SHF"
                  value={symbol}
                  onChange={(e) => setSymbol(e.target.value.toUpperCase())}
                />
              </label>
            )}
            {capability?.date_range && (
              <>
                <label>
                  开始日期
                  <input
                    required
                    aria-label="开始日期"
                    type="date"
                    max={end}
                    value={start}
                    onChange={(e) => setStart(e.target.value)}
                  />
                </label>
                <label>
                  结束日期
                  <input
                    required
                    aria-label="结束日期"
                    type="date"
                    min={start}
                    max={today()}
                    value={end}
                    onChange={(e) => setEnd(e.target.value)}
                  />
                </label>
              </>
            )}
            <button
              className="primary"
              disabled={busy || !connected || !provider?.configured}
            >
              {busy ? "提交中…" : "开始同步"}
            </button>
          </>
        )}
      </form>
      {providerId === "local_file" ? (
        <DataImport
          token={token}
          connected={connected}
          onSubmitted={onSubmitted}
        />
      ) : (
        <div className="panel-footnote">
          {capability?.description} ·{" "}
          {provider?.configured
            ? "已保存凭据；接口权限以同步结果为准"
            : "请在设置 → 数据源中配置 Token"}
        </div>
      )}
      {error && (
        <div className="alert" role="alert">
          {error}
        </div>
      )}
      {providerId === "local_file" && (
        <div className="panel-footnote">
          导入成功后，结果会显示在数据集。
          <button onClick={onBrowse}>查看数据集</button>
        </div>
      )}
      {notice && (
        <div className="notice" role="status">
          {notice}
          <button onClick={onBrowse}>查看数据集</button>
        </div>
      )}
    </>
  );
}
