import { useEffect, useId, useState } from "react";
import type { components } from "@asterion/api-types/schema";
import type { RequestClient } from "@asterion/runtime-client/requests";
import "./roles.css";

type Candidate = components["schemas"]["CandidateEvidence"];
type Spec = Version["spec"];
type Version = components["schemas"]["ComputedVersion"];
type Time = components["schemas"]["TimeVersion"];
type Policy = Spec["request"]["policy"];
type Choice = { id: string; manifest: { source: string; scope?: { symbol?: string; exchange?: string; start?: string; end?: string }; contract_identity?: { symbol: string } } };
type Catalog = { items: Choice[]; total: number };
const message = (e: unknown) => e instanceof Error ? e.message : String(e);

export function StartCalculation({ api, enabled, onPublished, onCancel }: {
  api: RequestClient; enabled: boolean; onPublished: (v: Version) => void; onCancel: () => void;
}) {
  const id = useId();
  const [contracts, setContracts] = useState<Choice[]>([]);
  const [times, setTimes] = useState<Time[]>([]);
  const [daily, setDaily] = useState<Choice[]>([]);
  const [dailyTotal, setDailyTotal] = useState(0);
  const [loading, setLoading] = useState(false);
  const [reload, setReload] = useState(0);
  const [loadError, setLoadError] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [contractsId, setContractsId] = useState("");
  const [timeId, setTimeId] = useState("");
  const [day, setDay] = useState("");
  const [scope, setScope] = useState<"" | "explicit" | "product_catalog">("");
  const [symbols, setSymbols] = useState("");
  const [candidate, setCandidate] = useState<Candidate>();
  const [inputs, setInputs] = useState<Record<string, string>>({});
  const [metric, setMetric] = useState<Policy["metric"] | "">("");
  const [tie, setTie] = useState<Policy["tie_break"] | "">("");
  const [idle, setIdle] = useState<Policy["no_trade"] | "">("");
  const [margin, setMargin] = useState("");
  const [confirmations, setConfirmations] = useState("");
  const [backward, setBackward] = useState("");
  const [initial, setInitial] = useState("");
  const [explanation, setExplanation] = useState("");
  const [preview, setPreview] = useState<Spec>();
  const [attempted, setAttempted] = useState(false);
  const time = times.find(t => t.id === timeId);
  const disabled = busy || !enabled;

  useEffect(() => {
    if (!enabled) return;
    let live = true;
    setLoading(true); setLoadError("");
    void Promise.all([
      api.request<Catalog>("/data/catalog?type_id=futures.contracts&layer=STANDARD&limit=100"),
      api.request<Time[]>("/trading-time"),
      api.request<Catalog>("/data/catalog?type_id=futures.daily&layer=STANDARD&limit=100"),
    ]).then(([c, t, d]) => {
      if (live) { setContracts(c.items); setTimes(t); setDaily(d.items); setDailyTotal(d.total); }
    }).catch(e => { if (live) setLoadError(message(e)); })
      .finally(() => { if (live) setLoading(false); });
    return () => { live = false; };
  }, [api, enabled, reload]);

  async function candidates() {
    if (!time || !scope) return;
    setBusy(true); setError("");
    try {
      const value = await api.request<Candidate>("/contract-roles/candidates/preview", {
        contracts_version_id: contractsId.trim(), product_id: `${time.spec.exchange}.${time.spec.product}`,
        scope: { mode: scope, symbols: scope === "explicit" ? symbols.trim().split(/[\s,，]+/).filter(Boolean) : [] },
        start: day, end: day,
      });
      setCandidate(value); setInputs({}); setInitial(""); setPreview(undefined);
    } catch (e) { setError(message(e)); } finally { setBusy(false); }
  }

  async function calculate() {
    if (!candidate || !time || !metric || !tie || !idle || !initial || !backward) return;
    setBusy(true); setError(""); setPreview(undefined); setAttempted(false);
    try {
      const value = await api.request<Spec>("/contract-roles/computed/preview", {
        schema_version: 1, contracts_version_id: candidate.request.contracts_version_id,
        product_id: candidate.request.product_id, candidate_scope: candidate.request.scope,
        trading_time: time, initial_main: initial === "none" ? null : initial,
        daily_inputs: candidate.included.map(contract => ({ trading_day: candidate.request.start, version_id: inputs[contract]?.trim() })),
        policy: { metric, unit: "contracts", tie_break: tie, missing: "reject", no_trade: idle,
          switch_margin: margin, confirmations: Number(confirmations), allow_backward: backward === "yes", secondary: "best_remaining" },
        explanation: explanation.trim(),
      });
      setPreview(value);
    } catch (e) { setError(message(e)); } finally { setBusy(false); }
  }

  async function publish() {
    if (!preview) return;
    setBusy(true); setError(""); setAttempted(true);
    try { onPublished(await api.request<Version>("/contract-roles/computed/start", preview)); }
    catch (e) { setError(`发布未确认，可重试相同结果。${message(e)}`); }
    finally { setBusy(false); }
  }

  async function moreDaily() {
    setBusy(true); setError("");
    try {
      const result = await api.request<Catalog>(`/data/catalog?type_id=futures.daily&layer=STANDARD&limit=100&offset=${daily.length}`);
      setDaily(rows => [...rows, ...result.items]); setDailyTotal(result.total);
    } catch (e) { setError(message(e)); } finally { setBusy(false); }
  }

  return <section className="role-start" aria-label="首次计算主力角色">
    <div className="panel-heading"><h3>首次计算主力角色</h3><span className="panel-spacer" /><button disabled={busy} onClick={onCancel}>关闭</button></div>
    <p>固定一个交易日的完整日线，预览后发布主力与次主力。历史下载按本机实际可知时间计算，不回填过去开盘。</p>
    {loading && <p role="status">正在读取固定输入选项…</p>}
    {loadError && <p role="alert">{loadError} <button disabled={disabled || loading} onClick={() => setReload(v => v + 1)}>重试读取选项</button></p>}
    {!loading && !loadError && !times.length && <p role="status">没有交易时间版本。请先在数据导入的“交易时间”中配置品种时段与完整日历，再重新读取选项。<button disabled={disabled} onClick={() => setReload(v => v + 1)}>重新读取选项</button></p>}
    <div hidden={!!preview}>
    <form onSubmit={e => { e.preventDefault(); void candidates(); }}>
      <fieldset disabled={disabled || loading || !!candidate}><legend>品种与观测日</legend><div className="role-fields">
        <label>合约资料版本<input required list={`${id}-contracts`} value={contractsId} onChange={e => setContractsId(e.target.value)} placeholder="选择已同步资料，或粘贴固定版本 ID" /></label>
        <datalist id={`${id}-contracts`}>{contracts.map(c => <option key={c.id} value={c.id}>{c.manifest.source} · {c.manifest.scope?.exchange}</option>)}</datalist>
        <label>交易时间版本<select aria-label="交易时间版本" required value={timeId} onChange={e => setTimeId(e.target.value)}><option value="" disabled>选择品种时段与日历</option>{times.map(t => <option key={t.id} value={t.id}>{t.spec.exchange}.{t.spec.product} · {t.spec.title} · {t.id.slice(0, 8)}</option>)}</select></label>
        <label>观测交易日<input required type="date" value={day} onChange={e => setDay(e.target.value)} /></label>
        <label>候选范围<select aria-label="候选范围" required value={scope} onChange={e => setScope(e.target.value as typeof scope)}><option value="" disabled>请选择范围</option><option value="product_catalog">固定资料中的全部品种合约</option><option value="explicit">明确指定来源代码子集</option></select></label>
        {scope === "explicit" && <label>候选来源代码<input required value={symbols} onChange={e => setSymbols(e.target.value)} placeholder="至少两个，以空格或逗号分隔" /></label>}
      </div><p>资料选项列出前 100 个数据集的最新版本，也可粘贴历史版本 ID。缺少资料或日线时，请先在“数据同步”采集；资料范围不代表交易所全市场完备。</p>
      <button type="submit">核对候选合约</button></fieldset>
    </form>
    {candidate && <>
      {!preview && <button disabled={disabled} onClick={() => { setCandidate(undefined); setInputs({}); setError(""); }}>更改品种或观测日</button>}
      <form onSubmit={e => { e.preventDefault(); void calculate(); }}><fieldset disabled={disabled || !!preview}><legend>固定日线与排名规则</legend>
        <p>{candidate.request.product_id} · {candidate.request.start} · 当日存续 {candidate.included.length} 个合约；缺失行情将拒绝计算。</p>
        <div className="role-fields">{candidate.included.map((contract, index) => {
          const symbol = candidate.catalog.symbols.find(s => s.contract_id === contract)?.symbol;
          const suggestions = daily.filter(d => d.manifest.source === candidate.catalog.inputs[0].source && (d.manifest.contract_identity?.symbol ?? d.manifest.scope?.symbol) === symbol);
          return <label key={contract}>{symbol} 日线版本<input aria-label={`${symbol} 日线版本`} required list={`${id}-daily-${index}`} value={inputs[contract] ?? ""} onChange={e => setInputs(v => ({ ...v, [contract]: e.target.value }))} placeholder="选择或粘贴固定日线版本 ID" /><datalist id={`${id}-daily-${index}`}>{suggestions.map(d => <option key={d.id} value={d.id}>{d.manifest.scope?.start}—{d.manifest.scope?.end} · {d.id.slice(0, 12)}</option>)}</datalist></label>;
        })}</div>
        {daily.length < dailyTotal && <button type="button" onClick={() => void moreDaily()}>加载更多日线版本（已载入 {daily.length}/{dailyTotal}）</button>}
        <div className="role-fields">
          <label>排名指标<select aria-label="排名指标" required value={metric} onChange={e => setMetric(e.target.value as typeof metric)}><option value="" disabled>请选择指标</option><option value="open_interest">持仓量（手）</option><option value="volume">成交量（手）</option></select></label>
          <label>同值排序<select aria-label="同值排序" required value={tie} onChange={e => setTie(e.target.value as typeof tie)}><option value="" disabled>请选择排序</option><option value="earlier_delivery">较早交割月优先</option><option value="later_delivery">较晚交割月优先</option></select></label>
          <label>无成交处理<select aria-label="无成交处理" required value={idle} onChange={e => setIdle(e.target.value as typeof idle)}><option value="" disabled>请选择处理方式</option><option value="reject">拒绝计算</option><option value="exclude">排除无成交合约</option></select></label>
          <label>换月超越比例<input required type="number" min="0" step="any" value={margin} onChange={e => setMargin(e.target.value)} placeholder="例如 0.1 表示严格超越 10%" /></label>
          <label>连续确认次数<input required type="number" min="1" max="100" step="1" value={confirmations} onChange={e => setConfirmations(e.target.value)} /></label>
          <label>允许回到更早交割月<select aria-label="允许回到更早交割月" required value={backward} onChange={e => setBackward(e.target.value)}><option value="" disabled>请选择</option><option value="no">不允许</option><option value="yes">允许</option></select></label>
          <label>初始主力<select aria-label="初始主力" required value={initial} onChange={e => setInitial(e.target.value)}><option value="" disabled>请选择初始状态</option><option value="none">无，从本次排名开始</option>{candidate.included.map(c => <option key={c} value={c}>{candidate.catalog.symbols.find(s => s.contract_id === c)?.symbol} · {c}</option>)}</select></label>
          <label>计算依据说明<input required maxLength={2000} value={explanation} onChange={e => setExplanation(e.target.value)} /></label>
        </div><p>缺失行情一律拒绝；次主力取排除最终主力后的最高排名。同交割月同值时按规范身份排序。</p>
        <button type="submit">预览计算结果</button>
      </fieldset></form>
    </>}
    </div>
    {preview && <section aria-label="首次计算预览">
      <h3>待发布结果</h3>{preview.result.decisions.map(d => <div key={d.observation_day}>
        <dl className="role-summary"><div><dt>主力</dt><dd>{d.main}</dd></div><div><dt>次主力</dt><dd>{d.secondary}</dd></div><div><dt>生效交易日</dt><dd>{d.effective_day}</dd></div><div><dt>生效开盘</dt><dd>{d.effective_start}</dd></div><div><dt>输入可知时间</dt><dd>{d.available_at}</dd></div></dl>
        <p>发布必须不晚于以上开盘，最终以服务端校验为准。角色不授权下单。</p>
      </div>)}
      <details><summary>查看候选与固定证据</summary><pre>{JSON.stringify({ candidates: preview.candidates, decisions: preview.result.decisions, inputs: preview.request.daily_inputs, policy: preview.request.policy, algorithm: preview.artifact.algorithm, checksum: preview.artifact.checksum }, null, 2)}</pre></details>
      <div className="panel-heading"><button disabled={disabled} onClick={() => void publish()}>{busy ? "发布中…" : attempted ? "重试发布相同结果" : "确认发布"}</button><button disabled={disabled || attempted} onClick={() => setPreview(undefined)}>修改输入</button></div>
    </section>}
    {busy && !preview && <p role="status">正在核验固定来源…</p>}
    {error && <p role="alert">{error}</p>}
  </section>;
}
