import { useEffect, useState } from "react";
import type { RequestClient } from "@asterion/runtime-client/requests";
import type { components } from "@asterion/api-types/schema";
import type { Job } from "@asterion/ui-task-center/public";
import type { Provider } from "./client";
import { referenceVersions, type ReferenceVersion } from "./referenceVersions";
import "./history.css";

type Plan = components["schemas"]["HistoryPlan"];
type Batch = components["schemas"]["HistoryBatch"];
type Summary = components["schemas"]["HistorySummary"];
type Coverage = components["schemas"]["HistoryCoverage"];
type Contract = {
  symbol: string;
  product: string;
  delivery_month: string | null;
  listed: string;
  delisted: string | null;
};
const statuses: Record<string, string> = {
  COVERED: "记录齐全",
  GAPS: "存在缺口",
  UNCONFIRMED: "依据不足",
  CONFLICT: "数据冲突",
  NO_EXPECTED_ROWS: "无预期行情",
};
const versionLabel = (v: ReferenceVersion) =>
  `${new Date(v.created_at * 1000).toLocaleString()} · ${v.id.slice(0, 12)}`;
const today = () =>
  new Intl.DateTimeFormat("en-CA", { timeZone: "Asia/Shanghai" }).format(
    new Date(),
  );

export function HistoryDownloads({
  api,
  connected,
  providers,
  busy,
  setBusy,
  onSubmitted,
  onInspect,
}: {
  api: RequestClient;
  connected: boolean;
  providers: Provider[];
  busy: boolean;
  setBusy: (value: boolean) => void;
  onSubmitted: (job: Job) => void;
  onInspect: (versionId: string, reportId: string) => void;
}) {
  const eligible = providers.filter(
    (p) => !p.demo && p.capabilities.some((c) => c.type_id === "futures.daily"),
  );
  const [owner, setOwner] = useState("");
  const provider = eligible.find((p) => p.id === owner);
  const source = provider?.plugin_id ?? provider?.id;
  const connection = provider?.connection_id ?? undefined;
  const exchanges = [
    ...new Set(
      provider?.capabilities
        .filter((c) => c.type_id === "futures.daily")
        .flatMap((c) => c.exchanges),
    ),
  ];
  const [exchange, setExchange] = useState("");
  const [references, setReferences] = useState<
    [ReferenceVersion[], ReferenceVersion[]]
  >([[], []]);
  const [contractsId, setContractsId] = useState("");
  const [calendarId, setCalendarId] = useState("");
  const [contracts, setContracts] = useState<Contract[]>([]);
  const [symbols, setSymbols] = useState<string[]>([]);
  const [filter, setFilter] = useState("");
  const [start, setStart] = useState(`${new Date().getFullYear()}-01-01`);
  const [end, setEnd] = useState(today);
  const [revision, setRevision] = useState(0);
  const [loading, setLoading] = useState(false);
  const [loadingContracts, setLoadingContracts] = useState(false);
  const [referenceError, setReferenceError] = useState("");
  const [contractError, setContractError] = useState("");
  const [error, setError] = useState("");
  const [recent, setRecent] = useState<Summary[]>([]);
  const [batch, setBatch] = useState<Batch | null>(null);
  const [plan, setPlan] = useState<Plan | null>(null);
  const [coverage, setCoverage] = useState<Coverage | null>(null);
  const [editing, setEditing] = useState(true);
  const [attempted, setAttempted] = useState(false);

  useEffect(() => {
    if (!provider && eligible.length) setOwner(eligible[0].id);
  }, [provider, eligible]);
  useEffect(() => {
    setExchange(exchanges[0] ?? "");
  }, [owner]);
  useEffect(() => {
    let live = true;
    setBatch(null);
    setCoverage(null);
    setPlan(null);
    setEditing(true);
    setAttempted(false);
    setRecent([]);
    if (connected)
      void api
        .request<Summary[]>("/data/history")
        .then((values) => {
          if (live) setRecent(values);
        })
        .catch((e) => {
          if (live) setError(String(e));
        });
    return () => {
      live = false;
    };
  }, [api, connected]);
  useEffect(() => {
    let live = true;
    setReferences([[], []]);
    setContractsId("");
    setCalendarId("");
    setReferenceError("");
    if (!connected || !source || !exchange) {
      setLoading(false);
      return;
    }
    setLoading(true);
    void Promise.all([
      referenceVersions(
        api,
        "futures.contracts",
        source,
        connection,
        exchange,
        () => live,
      ),
      referenceVersions(
        api,
        "futures.calendar",
        source,
        connection,
        exchange,
        () => live,
      ),
    ])
      .then((values) => {
        if (live) setReferences(values);
      })
      .catch((e) => {
        if (live) setReferenceError(String(e));
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    return () => {
      live = false;
    };
  }, [api, connected, source, connection, exchange, revision]);
  useEffect(() => {
    let live = true;
    setContracts([]);
    setSymbols([]);
    setContractError("");
    if (!contractsId || !connected) {
      setLoadingContracts(false);
      return;
    }
    setLoadingContracts(true);
    async function load() {
      const rows: Contract[] = [];
      for (let offset = 0; live; offset += 500) {
        const page = await api.request<{ total: number; rows: Contract[] }>(
          `/data/versions/${contractsId}?offset=${offset}&limit=500`,
        );
        if (page.total > 10000)
          throw new Error("合约资料超过当前上限，请选择范围更小的资料版本");
        rows.push(...page.rows);
        if (offset + 500 >= page.total) break;
      }
      if (live) setContracts(rows);
    }
    void load()
      .catch((e) => {
        if (live) setContractError(String(e));
      })
      .finally(() => {
        if (live) setLoadingContracts(false);
      });
    return () => {
      live = false;
    };
  }, [api, connected, contractsId, revision]);

  async function run(action: () => Promise<void>) {
    if (busy || !connected) return;
    setBusy(true);
    setError("");
    try {
      await action();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  async function open(id: string) {
    const value = await api.request<Batch>(`/data/history/${id}`);
    setBatch(value);
    setCoverage(null);
    setPlan(null);
    setAttempted(false);
    setEditing(false);
  }
  const selectable = contracts.filter(
    (c) =>
      c.delivery_month && c.delisted && c.listed <= end && c.delisted >= start,
  );
  const visible = selectable
    .filter((c) =>
      `${c.symbol} ${c.product} ${c.delivery_month}`
        .toLowerCase()
        .includes(filter.toLowerCase()),
    )
    .slice(0, 100);
  const selected = new Set(symbols);
  const unavailable =
    !connected ||
    !provider?.configured ||
    loading ||
    loadingContracts ||
    !!referenceError ||
    !!contractError;
  const sourceLabel = (request: Plan["request"]) =>
    providers.find((p) => p.id === (request.connection_id ?? request.provider))
      ?.name ??
    request.connection_id ??
    request.provider;
  const request = batch?.plan.request;
  const counts =
    batch?.tasks.reduce<Record<string, number>>((out, job) => {
      out[job.state] = (out[job.state] ?? 0) + 1;
      return out;
    }, {}) ?? {};
  return (
    <section className="history-downloads" aria-label="批量历史下载">
      <div className="panel-heading">
        <h3>批量历史下载</h3>
        <span className="panel-spacer" />
        <button
          disabled={busy || !connected}
          onClick={() =>
            void run(async () => {
              setRecent(await api.request<Summary[]>("/data/history"));
            })
          }
        >
          刷新下载记录
        </button>
        {editing && batch && !plan && (
          <button
            disabled={busy}
            onClick={() => {
              setEditing(false);
              setError("");
            }}
          >
            返回下载结果
          </button>
        )}
        {!editing && (
          <button
            disabled={busy}
            onClick={() => {
              setEditing(true);
              setPlan(null);
              setAttempted(false);
              setError("");
            }}
          >
            新建下载
          </button>
        )}
      </div>
      {!!recent.length && (
        <label className="history-recent">
          最近下载（最多 20 批）
          <select
            aria-label="最近下载"
            value={request?.command_id ?? ""}
            disabled={busy || !connected}
            onChange={(e) => void run(() => open(e.target.value))}
          >
            <option value="" disabled>
              选择下载记录
            </option>
            {recent.map((v) => (
              <option key={v.request.command_id} value={v.request.command_id}>
                {sourceLabel(v.request)} · {v.request.exchange} ·{" "}
                {v.request.symbols.length} 合约 · {v.request.start}—
                {v.request.end} ·{" "}
                {new Date(v.created_at * 1000).toLocaleString()}
              </option>
            ))}
          </select>
        </label>
      )}
      {editing && !plan && (
        <form
          onSubmit={(e) => {
            e.preventDefault();
            void run(async () => {
              if (
                unavailable ||
                !source ||
                !symbols.length ||
                symbols.length > 100
              )
                return;
              setPlan(
                await api.request<Plan>("/data/history/plan", {
                  command_id: crypto.randomUUID(),
                  provider: source,
                  connection_id: connection ?? null,
                  exchange,
                  contracts_version_id: contractsId,
                  calendar_version_id: calendarId,
                  symbols: [...symbols].sort(),
                  start,
                  end,
                }),
              );
            });
          }}
        >
          <fieldset disabled={busy || !connected}>
            <div className="sync-form">
              <label>
                数据源
                <select
                  aria-label="批量下载数据源"
                  value={owner}
                  onChange={(e) => setOwner(e.target.value)}
                  required
                >
                  <option value="" disabled>
                    选择数据源
                  </option>
                  {eligible.map((p) => (
                    <option key={p.id} value={p.id}>
                      {p.name}
                      {p.configured ? "" : " · 待配置"}
                    </option>
                  ))}
                </select>
              </label>
              <label>
                交易所
                <select
                  aria-label="批量下载交易所"
                  value={exchange}
                  onChange={(e) => setExchange(e.target.value)}
                  required
                >
                  {exchanges.map((x) => (
                    <option key={x}>{x}</option>
                  ))}
                </select>
              </label>
              <label>
                开始日期
                <input
                  aria-label="批量开始日期"
                  type="date"
                  required
                  max={end}
                  value={start}
                  onChange={(e) => {
                    setStart(e.target.value);
                    setSymbols([]);
                  }}
                />
              </label>
              <label>
                结束日期
                <input
                  aria-label="批量结束日期"
                  type="date"
                  required
                  min={start}
                  max={today()}
                  value={end}
                  onChange={(e) => {
                    setEnd(e.target.value);
                    setSymbols([]);
                  }}
                />
              </label>
              <label>
                合约资料
                <select
                  aria-label="批量合约资料"
                  required
                  value={contractsId}
                  disabled={loading}
                  onChange={(e) => setContractsId(e.target.value)}
                >
                  <option value="">选择固定合约资料</option>
                  {references[0].map((v) => (
                    <option key={v.id} value={v.id}>
                      {versionLabel(v)}
                    </option>
                  ))}
                </select>
              </label>
              <label>
                交易日历
                <select
                  aria-label="批量交易日历"
                  required
                  value={calendarId}
                  disabled={loading}
                  onChange={(e) => setCalendarId(e.target.value)}
                >
                  <option value="">选择固定交易日历</option>
                  {references[1].map((v) => (
                    <option key={v.id} value={v.id}>
                      {versionLabel(v)}
                      {v.manifest.first
                        ? ` · ${v.manifest.first}—${v.manifest.last}`
                        : ""}
                    </option>
                  ))}
                </select>
              </label>
              <button
                type="button"
                onClick={() => setRevision((v) => v + 1)}
                disabled={loading}
              >
                刷新资料
              </button>
            </div>
            {(loading || loadingContracts) && (
              <p role="status">正在读取固定资料…</p>
            )}
            {!loading && (!references[0].length || !references[1].length) && (
              <p>请先通过单合约同步入口采集同一连接的合约资料和交易日历。</p>
            )}
            {referenceError && (
              <p role="alert">{referenceError} · 可刷新资料重试</p>
            )}
            {contractError && (
              <p role="alert">{contractError} · 可刷新资料重试</p>
            )}
            {contractsId && !loadingContracts && !contractError && (
              <>
                <div className="panel-heading">
                  <label>
                    搜索合约
                    <input
                      aria-label="搜索批量合约"
                      value={filter}
                      onChange={(e) => setFilter(e.target.value)}
                      placeholder="品种、来源代码或交割月"
                    />
                  </label>
                  <span>已选 {symbols.length} / 100</span>
                  <button
                    type="button"
                    onClick={() =>
                      setSymbols([...new Set(visible.map((c) => c.symbol))])
                    }
                  >
                    选择当前显示
                  </button>
                  <button type="button" onClick={() => setSymbols([])}>
                    清空选择
                  </button>
                </div>
                <div className="history-contracts">
                  <table>
                    <thead>
                      <tr>
                        <th>选择</th>
                        <th>来源代码</th>
                        <th>品种</th>
                        <th>交割月</th>
                        <th>上市至最后交易日</th>
                      </tr>
                    </thead>
                    <tbody>
                      {visible.map((c) => (
                        <tr key={`${c.symbol}:${c.listed}`}>
                          <td>
                            <input
                              type="checkbox"
                              aria-label={`选择 ${c.symbol} ${c.listed}`}
                              checked={selected.has(c.symbol)}
                              disabled={
                                !selected.has(c.symbol) && symbols.length >= 100
                              }
                              onChange={(e) =>
                                setSymbols(
                                  e.target.checked
                                    ? [...new Set([...symbols, c.symbol])]
                                    : symbols.filter((s) => s !== c.symbol),
                                )
                              }
                            />
                          </td>
                          <td>{c.symbol}</td>
                          <td>{c.product}</td>
                          <td>{c.delivery_month}</td>
                          <td>
                            {c.listed}—{c.delisted}
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
                {!visible.length && (
                  <p>没有符合日期、搜索条件且身份资料完整的合约。</p>
                )}
                <p className="panel-footnote">
                  显示至多 100
                  行；可搜索缩小范围。仅列出生命周期与所选日期相交、交割月及最后交易日齐全的合约。
                </p>
              </>
            )}
            <button
              className="primary"
              disabled={
                unavailable || !contractsId || !calendarId || !symbols.length
              }
            >
              核对下载范围
            </button>
          </fieldset>
        </form>
      )}
      {editing && plan && (
        <div className="history-preview">
          <p>
            {sourceLabel(plan.request)} · {plan.request.exchange} ·{" "}
            {plan.request.symbols.length} 个合约 · {plan.request.start}—
            {plan.request.end}
          </p>
          <p>
            将提交 {plan.slices.length} 个按月下载任务，预计{" "}
            {plan.expected_days}{" "}
            个合约交易日；日期已按合约上市及最后交易日裁剪。
          </p>
          <details>
            <summary>查看合约与月份明细</summary>
            <div className="history-contracts">
              <table>
                <thead>
                  <tr>
                    <th>实际合约</th>
                    <th>开始</th>
                    <th>结束</th>
                    <th>交易日</th>
                  </tr>
                </thead>
                <tbody>
                  {plan.slices.map((s) => (
                    <tr key={s.request.command_id}>
                      <td>{s.contract_id}</td>
                      <td>{s.request.start}</td>
                      <td>{s.request.end}</td>
                      <td>{s.expected_days}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </details>
          <div className="panel-heading">
            <button
              className="primary"
              disabled={busy || !connected}
              onClick={() =>
                void run(async () => {
                  setAttempted(true);
                  const value = await api.request<Batch>(
                    "/data/history",
                    plan.request,
                  );
                  setBatch(value);
                  setCoverage(null);
                  setEditing(false);
                  setPlan(null);
                  setAttempted(false);
                  value.tasks.forEach(onSubmitted);
                  setRecent(await api.request<Summary[]>("/data/history"));
                })
              }
            >
              {busy ? "提交中…" : attempted ? "重试确认下载" : "确认下载"}
            </button>
            <button
              disabled={busy || attempted}
              onClick={() => {
                setPlan(null);
                setError("");
              }}
            >
              修改范围
            </button>
          </div>
          {attempted && !busy && (
            <p>提交状态尚未确认，请重试同一请求，或刷新下载记录查看。</p>
          )}
        </div>
      )}
      {!editing && batch && request && (
        <>
          <p>
            {request.exchange} · {request.symbols.length} 个合约 ·{" "}
            {request.start}—{request.end} · 来源 {sourceLabel(request)}
          </p>
          <p>
            下载任务：成功 {counts.SUCCEEDED ?? 0} · 等待 {counts.QUEUED ?? 0} ·
            运行 {counts.RUNNING ?? 0} · 失败 {counts.FAILED ?? 0} · 取消{" "}
            {counts.CANCELLED ?? 0}（含重试记录）
          </p>
          <div className="panel-heading">
            <button
              disabled={busy || !connected}
              onClick={() => void run(() => open(request.command_id))}
            >
              刷新进度
            </button>
            <button
              disabled={busy || !connected}
              onClick={() => batch.tasks.forEach(onSubmitted)}
            >
              查看任务与重试
            </button>
            <button
              className="primary"
              disabled={busy || !connected}
              onClick={() =>
                void run(async () => {
                  setCoverage(null);
                  setCoverage(
                    await api.request<Coverage>(
                      `/data/history/${request.command_id}/coverage`,
                      {},
                    ),
                  );
                })
              }
            >
              核对覆盖
            </button>
          </div>
          {!coverage && <p>任务成功不代表数据齐全；下载完成后请核对覆盖。</p>}
          {coverage && (
            <>
              <p role="status">
                {coverage.complete
                  ? "所选依据下记录齐全"
                  : "覆盖尚未齐全，请查看缺口或依据问题"}{" "}
                · 已有 {coverage.counts.PRESENT ?? 0} · 缺口{" "}
                {coverage.counts.GAP ?? 0}。结果来自本次核对。
              </p>
              <div className="history-contracts">
                <table>
                  <thead>
                    <tr>
                      <th>合约</th>
                      <th>覆盖状态</th>
                      <th>缺口</th>
                      <th>操作</th>
                    </tr>
                  </thead>
                  <tbody>
                    {coverage.items.map((item) => (
                      <tr key={item.symbol}>
                        <td title={item.contract_id}>{item.symbol}</td>
                        <td>
                          {item.error ??
                            (item.report
                              ? statuses[item.report.status]
                              : "尚无已发布数据")}
                        </td>
                        <td>{item.report?.counts.GAP ?? "—"}</td>
                        <td>
                          {item.report && (
                            <button
                              onClick={() =>
                                onInspect(
                                  item.report!.daily_version_id,
                                  item.report!.id,
                                )
                              }
                            >
                              查看缺口与依据
                            </button>
                          )}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </>
          )}
          <details>
            <summary>固定依据</summary>
            <p>合约资料：{request.contracts_version_id}</p>
            <p>交易日历：{request.calendar_version_id}</p>
            <p>下载记录：{request.command_id}</p>
          </details>
        </>
      )}
      {error && (
        <div className="alert" role="alert">
          {error}
        </div>
      )}
    </section>
  );
}
