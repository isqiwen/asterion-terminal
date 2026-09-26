import type { RequestClient } from "@asterion/runtime-client/requests";
import { useEffect, useRef, useState } from "react";

import { TimePicker, type TimeVersion } from "@asterion/ui-trading-time-controls/public";
import { openSettings } from "@asterion/workbench/settings/window";
import {
  ResearchPreparation,
  type Preparation,
} from "./ResearchPreparation";
import type { Job } from "@asterion/ui-task-center/public";
import { readProviders, type Provider } from "./client";
import { HistoryDownloads } from "./HistoryDownloads";
import { DataImport } from "./DataImport";
import { SyncIdentity } from "./SyncIdentity";

type SyncCapability = Provider["capabilities"][number];

const today = () =>
  new Intl.DateTimeFormat("en-CA", { timeZone: "Asia/Shanghai" }).format(
    new Date(),
  );

export function DataSync({
  api,
  connected,
  onSubmitted,
  onBrowse,
  onPrepared,
}: {
  api: RequestClient;
  connected: boolean;
  onSubmitted: (job: Job) => void;
  onBrowse: () => void;
  onPrepared: (versionId: string, reportId: string) => void;
}) {
  const [minuteTime, setMinuteTime] = useState<TimeVersion | null>(null);
  const [minuteBoundary, setMinuteBoundary] = useState<
    "bar_start" | "bar_end" | ""
  >("");
  const [minuteFrequency, setMinuteFrequency] = useState("");
  const [minuteEvidence, setMinuteEvidence] = useState("");
  const [contractsVersion, setContractsVersion] = useState("");
  const [preparing, setPreparing] = useState(false);
  const [recent, setRecent] = useState<Preparation | null>(null);
  const [providers, setProviders] = useState<Provider[]>([]);
  const [providerId, setProviderId] = useState("");
  const [history, setHistory] = useState(false);
  const [importing, setImporting] = useState(false);
  const [dataset, setDataset] = useState("");
  const [exchange, setExchange] = useState("");
  const [symbol, setSymbol] = useState("");
  useEffect(() => {
    setContractsVersion("");
  }, [providerId, dataset, exchange, symbol]);
  const [start, setStart] = useState(`${new Date().getFullYear()}-01-01`);
  const [end, setEnd] = useState(today);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const selectedProvider = useRef("");
  const command = useRef<{ key: string; id: string } | null>(null);
  const provider = providers.find((p) => p.id === providerId);
  const capability = provider?.capabilities.find((c) => c.id === dataset);
  const isMinute = capability?.type_id === "futures.minute";
  const identityRequired = [
    "futures.daily",
    "futures.settlement",
    "futures.minute",
  ].includes(capability?.type_id ?? "");
  const prepareSupported = [
    "futures.daily",
    "futures.calendar",
    "futures.contracts",
  ].every(
    (type) =>
      provider?.capabilities.filter(
        (c) => c.type_id === type && c.exchanges.includes(exchange),
      ).length === 1,
  );
  function selectCapability(selected: SyncCapability) {
    command.current = null;
    setPreparing(false);
    setDataset(selected.id);
    setExchange(selected.exchanges[0] ?? "");
    setSymbol(selected.defaults?.symbol ?? "");
    setStart(selected.defaults?.start ?? `${new Date().getFullYear()}-01-01`);
    setEnd(selected.defaults?.end ?? today());
  }
  useEffect(() => {
    if (
      providers.some((candidate) => candidate.id === selectedProvider.current)
    )
      return;
    const first =
      providers.find(
        (candidate) => candidate.configured && candidate.capabilities.length,
      ) ?? providers.find((candidate) => candidate.capabilities.length);
    if (first) {
      selectedProvider.current = first.id;
      setProviderId(first.id);
      selectCapability(first.capabilities[0]);
    }
  }, [providers, providerId]);
  useEffect(() => {
    if (!connected) return;
    let active = true;
    const refresh = () =>
      readProviders(api)
        .then((p) => {
          if (active)
            setProviders(
              p.filter((item) => item.lifecycle.state === "enabled"),
            );
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
  }, [api, connected]);
  async function submit(e: React.FormEvent) {
    e.preventDefault();
    if (
      busy ||
      !provider?.configured ||
      !capability ||
      (!preparing && identityRequired && !contractsVersion) ||
      (preparing && !prepareSupported) ||
      (isMinute &&
        (!minuteTime ||
          !minuteBoundary ||
          !minuteEvidence.trim() ||
          !capability?.frequencies?.includes(
            minuteFrequency as "1m" | "5m" | "15m" | "30m" | "60m",
          )))
    )
      return;
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const inputs = {
        provider: provider.plugin_id ?? providerId,
        ...(provider.connection_id
          ? { connection_id: provider.connection_id }
          : {}),
        dataset,
        exchange,
        symbol: capability?.symbol_required ? symbol.trim() : "",
        start: capability?.date_range ? start : null,
        end: capability?.date_range ? (isMinute ? start : end) : null,
        ...(isMinute
          ? {
              minute_context: {
                frequency: minuteFrequency,
                trading_time: minuteTime,
                timestamp_semantics: minuteBoundary,
                semantics_source: minuteEvidence.trim(),
              },
            }
          : {}),
      };
      const key = JSON.stringify({ inputs, preparing, contractsVersion });
      if (command.current?.key !== key)
        command.current = { key, id: crypto.randomUUID() };
      if (preparing) {
        const batch = await api.request<Preparation>("/data/preparations", {
          command_id: command.current.id,
          ...inputs,
        });
        setRecent(batch);
        batch.tasks.forEach(({ job }) => onSubmitted(job));
        setNotice(
          "研究数据准备已提交：先采集日历与合约资料，身份校验通过后自动采集日线。",
        );
      } else {
        const job = await api.request<Job>("/data/sync", {
          contracts_version_id: identityRequired ? contractsVersion : null,
          command_id: command.current.id,
          ...inputs,
        });
        onSubmitted(job);
        setNotice("同步任务已提交，进度与异常会显示在任务中心。");
      }
      command.current = null;
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <div className="panel-heading">
        <h2>{importing ? "导入数据" : "数据同步"}</h2>
        <span className="panel-spacer" />
        <small>历史数据</small>
        <button
          disabled={busy}
          onClick={() => {
            setImporting(false);
            setHistory(!history);
          }}
        >
          {history ? "单合约同步" : "批量下载"}
        </button>
        <button
          disabled={busy}
          onClick={() => {
            setHistory(false);
            setImporting(!importing);
          }}
        >
          {importing ? "返回数据同步" : "导入数据"}
        </button>
        <button
          onClick={() =>
            openSettings("数据源").catch((e) => setError(String(e)))
          }
        >
          数据源设置
        </button>
      </div>
      {!importing && !history && (
        <form className="sync-form" onSubmit={submit}>
          <label>
            数据源
            <select
              aria-label="数据源"
              value={providerId}
              disabled={busy}
              onChange={(e) => {
                const id = e.currentTarget.value;
                selectedProvider.current = id;
                command.current = null;
                setError("");
                setNotice("");
                setProviderId(id);
                const p = providers.find((candidate) => candidate.id === id)!;
                if (p.capabilities[0]) selectCapability(p.capabilities[0]);
              }}
            >
              {!providers.length && (
                <option value="" disabled>
                  数据源待连接
                </option>
              )}
              {providers.map((p) => (
                <option key={p.id} value={p.id}>
                  {p.name} · {p.configured ? "已配置" : "待配置"}
                </option>
              ))}
            </select>
          </label>
          {!importing && !history && (
            <>
              <label>
                数据类型
                <select
                  aria-label="数据类型"
                  value={dataset}
                  disabled={busy}
                  onChange={(e) => {
                    setError("");
                    setNotice("");
                    const c = provider?.capabilities.find(
                      (c) => c.id === e.target.value,
                    );
                    if (c) selectCapability(c);
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
                  disabled={busy}
                  onChange={(e) => {
                    command.current = null;
                    setExchange(e.target.value);
                    setPreparing(false);
                  }}
                >
                  {capability?.exchanges.map((x) => (
                    <option key={x}>{x}</option>
                  ))}
                </select>
              </label>
              {capability?.symbol_required && (
                <label>
                  来源代码
                  <input
                    required
                    aria-label="来源代码"
                    placeholder={capability.defaults?.symbol ?? "输入来源代码"}
                    value={symbol}
                    disabled={busy}
                    onChange={(e) => {
                      command.current = null;
                      setSymbol(e.target.value);
                    }}
                  />
                </label>
              )}
              {capability?.date_range && (
                <>
                  <label>
                    {isMinute ? "交易日" : "开始日期"}
                    <input
                      required
                      aria-label="开始日期"
                      type="date"
                      max={isMinute ? today() : end}
                      value={start}
                      disabled={busy}
                      onChange={(e) => {
                        command.current = null;
                        setStart(e.target.value);
                      }}
                    />
                  </label>
                  {!isMinute && (
                    <label>
                      结束日期
                      <input
                        required
                        aria-label="结束日期"
                        type="date"
                        min={start}
                        max={today()}
                        value={end}
                        disabled={busy}
                        onChange={(e) => {
                          command.current = null;
                          setEnd(e.target.value);
                        }}
                      />
                    </label>
                  )}
                </>
              )}
              {isMinute && (
                <div className="import-options">
                  <label>
                    分钟周期
                    <select
                      aria-label="分钟周期"
                      value={minuteFrequency}
                      onChange={(e) => setMinuteFrequency(e.target.value)}
                      disabled={busy}
                    >
                      <option value="">选择周期</option>
                      {(capability?.frequencies ?? []).map((f) => (
                        <option key={f} value={f}>
                          {f.replace("m", " 分钟")}
                        </option>
                      ))}
                    </select>
                  </label>
                  <TimePicker
                    api={api}
                    value={minuteTime}
                    onChange={setMinuteTime}
                    disabled={busy || !connected}
                  />
                  <label>
                    分钟时间戳
                    <select
                      aria-label="分钟时间戳含义"
                      required
                      value={minuteBoundary}
                      disabled={busy}
                      onChange={(e) =>
                        setMinuteBoundary(
                          e.target.value as typeof minuteBoundary,
                        )
                      }
                    >
                      <option value="">选择来源时间含义</option>
                      <option value="bar_end">分钟结束时刻</option>
                      <option value="bar_start">分钟开始时刻</option>
                    </select>
                  </label>
                  <label>
                    时间含义依据
                    <input
                      aria-label="分钟时间含义依据"
                      required
                      value={minuteEvidence}
                      disabled={busy}
                      onChange={(e) => setMinuteEvidence(e.target.value)}
                      placeholder="供应商说明或已核实的校验记录"
                    />
                  </label>
                  <small>
                    一次下载一个实际合约的一个交易日，包含固定时段内的夜盘。分钟权限需单独开通；缺失记录不自动补零。
                  </small>
                </div>
              )}
              {capability?.type_id === "futures.daily" && (
                <label className="sync-preparation-option">
                  <input
                    type="checkbox"
                    checked={preparing}
                    disabled={busy || !prepareSupported}
                    onChange={(e) => {
                      setPreparing(e.target.checked);
                      command.current = null;
                      setNotice("");
                    }}
                  />
                  同时准备研究依据（日历与合约资料）
                  {!prepareSupported && (
                    <small>
                      此连接不同时支持该交易所的三类数据，请分别同步。
                    </small>
                  )}
                </label>
              )}
              {identityRequired && !preparing && provider && (
                <SyncIdentity
                  key={`${providerId}:${dataset}:${exchange}:${symbol}`}
                  api={api}
                  source={provider.plugin_id ?? providerId}
                  connection={provider.connection_id ?? undefined}
                  exchange={exchange}
                  disabled={busy || !connected}
                  value={contractsVersion}
                  onChange={setContractsVersion}
                />
              )}
              <button
                className="primary"
                disabled={
                  busy ||
                  !connected ||
                  !provider?.configured ||
                  !capability ||
                  (preparing && !prepareSupported) ||
                  (isMinute &&
                    (!minuteTime ||
                      !minuteBoundary ||
                      !minuteEvidence.trim() ||
                      !capability?.frequencies?.includes(
                        minuteFrequency as "1m" | "5m" | "15m" | "30m" | "60m",
                      )))
                }
              >
                {busy ? "提交中…" : preparing ? "准备研究数据" : "开始同步"}
              </button>
            </>
          )}
        </form>
      )}
      {history && (
        <HistoryDownloads
          api={api}
          connected={connected}
          providers={providers}
          busy={busy}
          setBusy={setBusy}
          onSubmitted={onSubmitted}
          onInspect={onPrepared}
        />
      )}
      {history ? null : importing ? (
        <DataImport api={api} connected={connected} onSubmitted={onSubmitted} />
      ) : (
        <div className="panel-footnote">
          {provider?.demo && <strong>合成示例 · 非真实行情。 </strong>}
          {capability?.description} ·{" "}
          {provider?.configured
            ? "配置就绪；修改配置只影响后续任务"
            : "请在设置 → 数据源中完成配置"}
        </div>
      )}
      {!importing && !history && (
        <ResearchPreparation
          api={api}
          connected={connected}
          recent={recent}
          onSubmitted={onSubmitted}
          onInspect={onPrepared}
        />
      )}
      {error && (
        <div className="alert" role="alert">
          {error}
        </div>
      )}
      {importing && (
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
