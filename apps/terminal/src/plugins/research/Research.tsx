import { Experiments } from "./Experiments";
import type { ParameterValues } from "../../components/ParameterFields";
import { StrategyPicker, type StrategyRef } from "./StrategyPicker";
import {
  RulePicker,
  RuleEvidence,
  type RuleVersion,
} from "../contract_rules/public";
import type { RequestClient } from "../../api/requests";
import { useEffect, useRef, useState } from "react";

import { type CoverageReport } from "../data/client";
import type { Job } from "../tasks/public";
import { CoverageEvidence, ResearchCoverage } from "./ResearchCoverage";
import { ResearchExport, ResearchImport } from "./ResearchPackages";
import { ResearchRefill, type RefillState } from "./ResearchRefill";
import { ResearchWorkspace, type ResearchDraft } from "./ResearchWorkspace";

type Version = {
  id: string;
  dataset_id: string;
  rows: number;
  manifest: {
    version_semantics?: string;
    source: string;
    first: string;
    last: string;
    scope: Record<string, unknown>;
  };
};
export type Config = {
  version_id: string;
  start: string;
  end: string;
  parameters: ParameterValues;
  capital: string;
  rules: RuleVersion | null;
  slippage_ticks: number;
  lots: number;
  strategy: StrategyRef | null;
  assumption: string;
  coverage_policy: "require_complete" | "allow_incomplete";
  coverage_note: string;
  coverage_report_id?: string | null;
};
type Point = {
  day: string;
  session_open: string;
  session_close: string;
  equity: string;
  drawdown: string;
  position: number;
  margin: string;
  margin_rate: string;
  free_cash: string;
  settle: string;
  opening_balance: string;
  settlement_pnl: string;
  fees: string;
  balance: string;
  close_pnl: string;
  close_equity: string;
};
type Summary = {
  final_equity: string;
  net_profit: string;
  return_rate: string;
  max_drawdown: string;
  fees: string;
  fill_count: number;
  open_lots: number;
};
type Run = {
  id: string;
  state: string;
  error: string | null;
  contract: string;
  request: Config;
  input_checksum: string;
  engine: string;
  result: Summary | null;
};
type Detail = Run & {
  reproduction?: {
    origin_run_id: string;
    expected_output_checksum: string;
  } | null;
  coverage?: CoverageReport | null;
  version: Version;
  warnings: string[];
  output: {
    summary: Summary;
    curve: Point[];
    fills: {
      day: string;
      time: string;
      side: string;
      lots: number;
      price: string;
      fee: string;
      fee_mode: string;
      fee_rate: string;
      rule_start: string;
      reason: string;
    }[];
    events: { day: string; reason: string }[];
  } | null;
};
const states: Record<string, string> = {
  QUEUED: "排队中",
  RUNNING: "运行中",
  SUCCEEDED: "已完成",
  FAILED: "失败",
  CANCELLED: "已取消",
};
const initial: Config = {
  version_id: "",
  start: "",
  end: "",
  parameters: {},
  capital: "100000",
  rules: null,
  slippage_ticks: 1,
  lots: 1,
  strategy: null,
  assumption: "historical-close-unverified-calendar",
  coverage_policy: "require_complete",
  coverage_note: "",
};
const number = (value: string | number) =>
  Number(value).toLocaleString("zh-CN", { maximumFractionDigits: 2 });
const percent = (value: string) => `${(Number(value) * 100).toFixed(2)}%`;

function Equity({ points }: { points: Point[] }) {
  const values = points.map((p) => Number(p.equity));
  const min = Math.min(...values),
    max = Math.max(...values),
    span = max - min || 1;
  return (
    <figure className="research-equity">
      <figcaption>
        每日结算权益 · {number(min)} — {number(max)}
      </figcaption>
      <svg viewBox="0 0 900 150" role="img" aria-label="回测权益曲线">
        <polyline
          fill="none"
          stroke="var(--accent, #ff9500)"
          strokeWidth="2"
          points={values
            .map(
              (v, i) =>
                `${(i * 900) / Math.max(1, values.length - 1)},${140 - ((v - min) / span) * 125}`,
            )
            .join(" ")}
        />
      </svg>
      <figcaption>
        {points[0]?.day} — {points.at(-1)?.day} · 精确数值见每日权益表
      </figcaption>
    </figure>
  );
}

export function Research({
  api,
  connected,
  onSubmitted,
  accountEmail,
  onInspectData,
  externalRefill,
}: {
  api: RequestClient;
  connected: boolean;
  accountEmail: string;
  onSubmitted: (job: Job) => void;
  onInspectData: (versionId: string, reportId: string) => void;
  externalRefill?: { reportId: string; jobs: Job[] } | null;
}) {
  const refillCache = useRef(new Map<string, RefillState>());
  const [strategyReady, setStrategyReady] = useState(false);
  const [workspaceReady, setWorkspaceReady] = useState(false);
  const [datasetId, setDatasetId] = useState("");
  const [datasets, setDatasets] = useState<Version[]>([]);
  const [versions, setVersions] = useState<Version[]>([]);
  const [runs, setRuns] = useState<Run[]>([]);
  const [config, setConfig] = useState(initial);
  const [report, setReport] = useState<CoverageReport | null>(null);
  const matchingReport =
    report?.daily_version_id === config.version_id &&
    report.start === config.start &&
    report.end === config.end
      ? report
      : null;
  const coverageReady =
    !!matchingReport?.identity &&
    matchingReport?.status !== "CONFLICT" &&
    (config.coverage_policy === "require_complete"
      ? matchingReport?.status === "COVERED"
      : !!config.coverage_note.trim());
  const [ack, setAck] = useState(false);
  useEffect(() => {
    setAck(false);
  }, [config.version_id, config.start, config.end]);
  const [selected, setSelected] = useState("");
  const [detail, setDetail] = useState<Detail | null>(null);
  const [compare, setCompare] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const generation = useRef(0);
  const command = useRef({ signature: "", id: "" });
  useEffect(() => {
    if (!connected) return;
    let live = true;
    api
      .request<{ items: Version[] }>(
        "/data/catalog?type_id=futures.daily&layer=STANDARD&limit=100",
      )
      .then((v) => {
        if (live)
          setDatasets((previous) => [
            ...v.items,
            ...previous.filter(
              (p) => !v.items.some((item) => item.dataset_id === p.dataset_id),
            ),
          ]);
      })
      .catch((e) => {
        if (live) setError(String(e));
      });
    return () => {
      live = false;
    };
  }, [api, connected]);
  useEffect(() => {
    if (!connected) return;
    let live = true;
    const refresh = async () => {
      try {
        const items = await api.request<Run[]>("/research/runs");
        const value = selected
          ? await api.request<Detail>(`/research/runs/${selected}`)
          : null;
        if (live) {
          setRuns(items);
          setDetail(value);
        }
      } catch (e) {
        if (live) setError(String(e));
      }
    };
    void refresh();
    const timer = setInterval(refresh, 3000);
    return () => {
      live = false;
      clearInterval(timer);
    };
  }, [api, connected, selected]);
  function chooseVersion(v?: Version) {
    if (v)
      setConfig((c) => ({
        ...c,
        version_id: v.id,
        start: v.manifest.first?.slice(0, 10) || "",
        end: v.manifest.last?.slice(0, 10) || "",
      }));
  }
  async function chooseDataset(id: string) {
    setDatasetId(id);
    const seq = ++generation.current;
    setVersions([]);
    setConfig((c) => ({ ...c, version_id: "" }));
    if (!id) return;
    try {
      const page = await api.request<{ items: Version[] }>(
        `/data/catalog/${id}/versions?limit=100`,
      );
      if (seq === generation.current) {
        setVersions(page.items);
        chooseVersion(page.items[0]);
      }
    } catch (e) {
      setError(String(e));
    }
  }
  async function submit(rerun?: string) {
    setBusy(true);
    setError("");
    const submission = {
      ...config,
      coverage_report_id: matchingReport?.id || null,
    };
    const signature = JSON.stringify(rerun ? { rerun } : submission);
    if (command.current.signature !== signature)
      command.current = { signature, id: crypto.randomUUID() };
    try {
      const job = await api.request<Job>(
        rerun ? `/research/runs/${rerun}/rerun` : "/research/runs",
        rerun
          ? { command_id: command.current.id }
          : { ...submission, command_id: command.current.id },
      );
      onSubmitted(job);
      setSelected(job.id);
      setDetail(null);
      command.current.signature = "";
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  const draftIdentity = useRef("");
  draftIdentity.current = JSON.stringify([
    datasetId,
    config.version_id,
    config.start,
    config.end,
  ]);
  async function adoptVersion(id: string) {
    const expected = draftIdentity.current;
    if (!matchingReport) throw new Error("原覆盖报告已变化，请重新核对");
    const evidence = matchingReport;
    setBusy(true);
    try {
      const value = await api.request<{ version: Version }>(
        `/data/versions/${id}?limit=1`,
      );
      if (value.version.dataset_id !== datasetId)
        throw new Error("候选版本不属于当前数据集");
      const checked = await api.request<CoverageReport>(
        `/data/versions/${id}/coverage`,
        {
          start: evidence.start,
          end: evidence.end,
          use_latest_daily: false,
          calendar_version_id: evidence.calendar_version_id,
          contracts_version_id: evidence.contracts_version_id,
          reference_symbol: evidence.reference_symbol,
        },
      );
      if (draftIdentity.current !== expected)
        throw new Error("研究输入已变化，请重新选择候选版本");
      setVersions((previous) => [
        value.version,
        ...previous.filter((v) => v.id !== id),
      ]);
      setConfig((c) => ({ ...c, version_id: id }));
      setReport(checked);
      setAck(false);
    } finally {
      setBusy(false);
    }
  }
  async function restoreDraft(value: ResearchDraft) {
    setAck(false);
    setReport(null);
    setVersions([]);
    setDatasetId("");
    setError("");
    setConfig(value.config);
    setSelected(value.selected_run_id || "");
    if (!value.config.version_id) return;
    try {
      const found = await api.request<{ version: Version }>(
        `/data/versions/${value.config.version_id}?limit=1`,
      );
      if (found.version.id !== value.config.version_id)
        throw new Error("数据版本响应不匹配");
      setVersions([found.version]);
      setDatasetId(found.version.dataset_id);
      setDatasets((previous) =>
        previous.some((v) => v.dataset_id === found.version.dataset_id)
          ? previous
          : [found.version, ...previous],
      );
      if (value.config.coverage_report_id) {
        const evidence = await api.request<CoverageReport>(
          `/data/coverage/${value.config.coverage_report_id}`,
        );
        if (
          evidence.daily_version_id !== value.config.version_id ||
          evidence.start !== value.config.start ||
          evidence.end !== value.config.end
        )
          throw new Error("原覆盖报告与草稿不匹配，请重新核对");
        setReport(evidence);
      }
    } catch (e) {
      setError(
        `草稿参数已恢复，原数据或覆盖依据无法核验：${String(e)}。请重新选择数据或核对覆盖。`,
      );
    }
  }
  const ruleScope = versions.find((v) => v.id === config.version_id)?.manifest
    .scope;
  const ruleContract =
    typeof ruleScope?.contract === "string"
      ? ruleScope.contract
      : typeof ruleScope?.exchange === "string" &&
          typeof ruleScope?.symbol === "string"
        ? `${ruleScope.exchange}.${ruleScope.symbol.split(".")[0]}`
        : "";
  const comparison = runs.find((r) => r.id === compare);
  return (
    <section className="research panel-scroll" aria-label="本地回测">
      <div className="panel-heading">
        <h2>实验与运行</h2>
        <small>单合约日线 · 做多 / 空仓</small>
      </div>
      <ResearchImport
        api={api}
        accountEmail={accountEmail}
        disabled={!connected || busy}
        onSubmitted={(job) => {
          onSubmitted(job);
          setSelected(job.id);
          setDetail(null);
        }}
      />
      <ResearchWorkspace
        accountEmail={accountEmail}
        api={api}
        connected={connected}
        value={{
          schema_version: 1,
          config: { ...config, coverage_report_id: matchingReport?.id || null },
          selected_run_id: selected,
        }}
        onRestore={restoreDraft}
        onReady={setWorkspaceReady}
      />
      <form
        onSubmit={(e) => {
          e.preventDefault();
          void submit();
        }}
      >
        <fieldset disabled={!workspaceReady || busy}>
          <div className="research-fields">
            <label>
              日线数据集
              <select
                required
                disabled={!connected || busy}
                onChange={(e) => void chooseDataset(e.target.value)}
                value={datasetId}
              >
                <option value="">选择标准日线数据集</option>
                {datasets.map((v) => (
                  <option key={v.dataset_id} value={v.dataset_id}>
                    {v.manifest.source} ·{" "}
                    {String(
                      v.manifest.scope.contract ||
                        v.manifest.scope.symbol ||
                        v.dataset_id.slice(0, 8),
                    )}{" "}
                    · {v.rows} 行
                  </option>
                ))}
              </select>
            </label>
            <label>
              固定数据版本
              <select
                required
                value={config.version_id}
                onChange={(e) =>
                  chooseVersion(versions.find((v) => v.id === e.target.value))
                }
              >
                <option value="">选择版本</option>
                {config.version_id &&
                  !versions.some((v) => v.id === config.version_id) && (
                    <option value={config.version_id}>
                      原版本待核验：{config.version_id.slice(0, 8)}
                    </option>
                  )}
                {versions.map((v) => (
                  <option key={v.id} value={v.id}>
                    {v.id.slice(0, 8)} · {v.rows} 行 ·{" "}
                    {v.manifest.first?.slice(0, 10)}—
                    {v.manifest.last?.slice(0, 10)}
                  </option>
                ))}
              </select>
            </label>
            {(
              [
                ["start", "开始日期"],
                ["end", "结束日期"],
              ] as const
            ).map(([key, label]) => (
              <label key={key}>
                {label}
                <input
                  required
                  type="date"
                  value={config[key]}
                  onChange={(e) =>
                    setConfig((c) => ({ ...c, [key]: e.target.value }))
                  }
                />
              </label>
            ))}
            {(
              [
                ["lots", "手数"],
                ["slippage_ticks", "单边滑点（跳）"],
              ] as const
            ).map(([key, label]) => (
              <label key={key}>
                {label}
                <input
                  required
                  type="number"
                  min={key === "slippage_ticks" ? 0 : 1}
                  step="1"
                  value={config[key]}
                  onChange={(e) =>
                    setConfig((c) => ({ ...c, [key]: Number(e.target.value) }))
                  }
                />
              </label>
            ))}
            {([["capital", "初始资金（元）"]] as const).map(([key, label]) => (
              <label key={key}>
                {label}
                <input
                  required
                  type="number"
                  min="0"
                  step="any"
                  value={config[key]}
                  placeholder="请填写"
                  onChange={(e) =>
                    setConfig((c) => ({ ...c, [key]: e.target.value }))
                  }
                />
              </label>
            ))}
          </div>
          <StrategyPicker
            api={api}
            connected={connected}
            value={config.strategy}
            parameters={config.parameters}
            onReady={setStrategyReady}
            onChange={(strategy, parameters) => {
              setConfig((c) => ({ ...c, strategy, parameters }));
              setAck(false);
            }}
          />
          <p>
            区间内按所选策略预热，预热后至少需要一根日线。信号在收盘计算，下一根日线开盘成交；日线须包含结算价，权益按日结算，期末不强制平仓。
          </p>
          <RulePicker
            contract={ruleContract}
            api={api}
            disabled={!connected || busy}
            value={config.rules}
            onChange={(rules) => {
              setConfig((c) => ({ ...c, rules }));
              setAck(false);
            }}
          />
          <ResearchCoverage
            key={`${config.version_id}:${config.start}:${config.end}:${api.key}`}
            api={api}
            versionId={config.version_id}
            start={config.start}
            end={config.end}
            local={
              versions.find((v) => v.id === config.version_id)?.manifest
                .source === "local_file"
            }
            supported={
              versions.find((v) => v.id === config.version_id)?.manifest
                .source === "local_file" ||
              versions.find((v) => v.id === config.version_id)?.manifest
                .version_semantics === "CUMULATIVE"
            }
            disabled={!connected || busy}
            report={matchingReport}
            onReport={setReport}
          />
          {matchingReport && (
            <ResearchRefill
              key={matchingReport.id}
              cache={refillCache.current}
              report={matchingReport}
              api={api}
              disabled={!connected || busy}
              onSubmitted={onSubmitted}
              external={externalRefill}
              onInspect={() =>
                onInspectData(
                  matchingReport.daily_version_id,
                  matchingReport.id,
                )
              }
              onAdopt={adoptVersion}
            />
          )}
          <label>
            数据完整性要求
            <select
              value={config.coverage_policy}
              onChange={(e) => {
                setConfig((c) => ({
                  ...c,
                  coverage_policy: e.target.value as Config["coverage_policy"],
                }));
                setAck(false);
              }}
            >
              <option value="require_complete">
                严格模式：需要覆盖核对通过
              </option>
              <option value="allow_incomplete">
                探索模式：接受缺失或未确认数据
              </option>
            </select>
          </label>
          <p>探索模式仅放宽行情完整性要求，仍须核对并固定合约身份目录。</p>
          {config.coverage_policy === "allow_incomplete" && (
            <label>
              探索模式原因
              <input
                required
                maxLength={500}
                value={config.coverage_note}
                onChange={(e) =>
                  setConfig((c) => ({ ...c, coverage_note: e.target.value }))
                }
                placeholder="说明为何接受未核验或不完整数据"
              />
            </label>
          )}
          <label className="research-ack">
            <input
              type="checkbox"
              checked={ack}
              onChange={(e) => setAck(e.target.checked)}
            />
            我接受历史收盘可用假设及所选数据完整性模式；费用、乘数和保证金由我确认。
          </label>
          <button
            type="submit"
            disabled={
              !connected ||
              busy ||
              !ack ||
              !config.rules ||
              !strategyReady ||
              !config.version_id ||
              !versions.some((v) => v.id === config.version_id) ||
              !coverageReady
            }
          >
            运行回测
          </button>
          {!datasets.length && (
            <p>暂无可选日线时，请先在数据模块同步或导入单一合约日线。</p>
          )}
        </fieldset>
      </form>
      <Experiments api={api} connected={connected}
        ready={workspaceReady && !busy && ack && !!config.rules && strategyReady && !!config.version_id && versions.some((v) => v.id === config.version_id) && coverageReady}
        base={{ ...config, coverage_report_id: matchingReport?.id || null }}
        onOpen={(id) => { setSelected(id); setDetail(null); }} />
      {error && <p role="alert">{error}</p>}
      <div className="panel-heading">
        <h2>运行记录</h2>
        <small>最近 100 次 · 自动刷新</small>
      </div>
      <table className="data-table">
        <thead>
          <tr>
            <th>运行</th>
            <th>合约</th>
            <th>区间</th>
            <th>策略</th>
            <th>状态</th>
            <th>净收益（元）</th>
          </tr>
        </thead>
        <tbody>
          {runs.map((r) => (
            <tr key={r.id}>
              <td>
                <button
                  onClick={() => {
                    setSelected(r.id);
                    setDetail(null);
                  }}
                >
                  {r.id.slice(0, 8)}
                </button>
              </td>
              <td>{r.contract}</td>
              <td>
                {r.request.start}—{r.request.end}
              </td>
              <td>
                {r.request.strategy?.id} ·{" "}
                {JSON.stringify(r.request.parameters)}
              </td>
              <td>{states[r.state] || r.state}</td>
              <td>{r.result ? number(r.result.net_profit) : "—"}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {!runs.length && <p>尚无运行记录</p>}
      {detail && (
        <article aria-label="回测结果">
          <div className="panel-heading">
            <h2>运行 {detail.id.slice(0, 8)}</h2>
            <button
              disabled={!connected || busy}
              onClick={() => void submit(detail.id)}
            >
              按原输入重跑
            </button>
            <button
              disabled={!!detail.reproduction}
              title={
                detail.reproduction
                  ? "导入运行请使用原输入重跑；修改实验前需在本机准备数据版本"
                  : undefined
              }
              onClick={() => {
                setConfig(detail.request);
                setReport(detail.coverage || null);
                setDatasetId(detail.version.dataset_id);
                setAck(false);
                setVersions([detail.version]);
              }}
            >
              载入参数修改
            </button>
          </div>
          <p className="mono">
            {detail.contract} · {detail.request.strategy?.id} ·{" "}
            {detail.request.strategy?.version} · {detail.engine}
          </p>
          <p className="research-hash">
            数据版本：{detail.request.version_id}
            <br />
            输入指纹：{detail.input_checksum}
          </p>
          {detail.request.rules && (
            <RuleEvidence value={detail.request.rules} />
          )}
          {detail.coverage ? (
            <CoverageEvidence
              key={detail.coverage.id}
              report={detail.coverage}
            />
          ) : (
            <p>此运行未绑定覆盖报告，数据完整性未核验。</p>
          )}
          <p>
            完整性模式：
            {detail.request.coverage_policy === "require_complete"
              ? "严格"
              : detail.request.coverage_policy === "allow_incomplete"
                ? "探索"
                : "不受支持的历史参数"}
            {detail.request.coverage_note &&
              ` · 原因：${detail.request.coverage_note}`}
          </p>
          <details>
            <summary>固定参数与模型边界</summary>
            <pre>{JSON.stringify(detail.request, null, 2)}</pre>
            {detail.warnings.map((w) => (
              <p key={w}>{w}</p>
            ))}
          </details>
          {detail.reproduction && (
            <p>
              导入复现 · 原运行 {detail.reproduction.origin_run_id} ·{" "}
              {detail.state === "SUCCEEDED"
                ? "结果校验和与原运行一致"
                : "完成后核对原结果校验和"}
            </p>
          )}
          {detail.output && (
            <ResearchExport
              key={detail.id}
              runId={detail.id}
              api={api}
              disabled={!connected || busy}
            />
          )}
          {detail.error && <p role="alert">{detail.error}</p>}
          {detail.output && (
            <>
              <label>
                对比运行
                <select
                  value={compare}
                  onChange={(e) => setCompare(e.target.value)}
                >
                  <option value="">选择已完成运行</option>
                  {runs
                    .filter((r) => r.id !== detail.id && r.result)
                    .map((r) => (
                      <option key={r.id} value={r.id}>
                        {r.id.slice(0, 8)} · {r.contract} ·{" "}
                        {r.request.strategy?.id}
                      </option>
                    ))}
                </select>
              </label>
              {comparison && (
                <p>
                  对比输入：{comparison.contract} · {comparison.request.start}—
                  {comparison.request.end} · 版本{" "}
                  {comparison.request.version_id.slice(0, 8)}
                  。不同资金、数据或参数下的指标仅供并列查看。
                </p>
              )}
              <table className="data-table">
                <thead>
                  <tr>
                    <th>指标</th>
                    <th>当前运行</th>
                    {comparison?.result && <th>对比运行</th>}
                  </tr>
                </thead>
                <tbody>
                  {(
                    [
                      ["final_equity", "期末权益"],
                      ["net_profit", "净收益"],
                      ["return_rate", "收益率"],
                      ["max_drawdown", "最大结算回撤"],
                      ["fees", "累计费用"],
                      ["fill_count", "成交笔数"],
                      ["open_lots", "期末持仓（手）"],
                    ] as const
                  ).map(([key, label]) => (
                    <tr key={key}>
                      <td>{label}</td>
                      <td>
                        {key === "return_rate" || key === "max_drawdown"
                          ? percent(String(detail.output!.summary[key]))
                          : number(detail.output!.summary[key])}
                      </td>
                      {comparison?.result && (
                        <td>
                          {key === "return_rate" || key === "max_drawdown"
                            ? percent(String(comparison.result[key]))
                            : number(comparison.result[key])}
                        </td>
                      )}
                    </tr>
                  ))}
                </tbody>
              </table>
              <Equity points={detail.output.curve} />
              <details>
                <summary>
                  每日结算与资金账（{detail.output.curve.length}）
                </summary>
                <div className="research-table">
                  <table className="data-table">
                    <thead>
                      <tr>
                        <th>交易日</th>
                        <th>时段起止（北京时间）</th>
                        <th>结算价</th>
                        <th>期初余额</th>
                        <th>结算盈亏</th>
                        <th>手续费</th>
                        <th>结算余额 / 权益</th>
                        <th>收盘估值差额</th>
                        <th>收盘权益</th>
                        <th>回撤</th>
                        <th>持仓</th>
                        <th>保证金</th>
                        <th>保证金比例</th>
                        <th>可用资金</th>
                      </tr>
                    </thead>
                    <tbody>
                      {detail.output.curve.map((p) => (
                        <tr key={p.day}>
                          <td>{p.day}</td>
                          <td>{p.session_open}<br/>{p.session_close}</td>
                          <td>{number(p.settle)}</td>
                          <td>{number(p.opening_balance)}</td>
                          <td>{number(p.settlement_pnl)}</td>
                          <td>{number(p.fees)}</td>
                          <td>{number(p.balance)}</td>
                          <td>{number(p.close_pnl)}</td>
                          <td>{number(p.close_equity)}</td>
                          <td>{percent(p.drawdown)}</td>
                          <td>{p.position}</td>
                          <td>{number(p.margin)}</td>
                          <td>{p.margin_rate}</td>
                          <td>{number(p.free_cash)}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              </details>
              <details open>
                <summary>成交记录（{detail.output.fills.length}）</summary>
                <div className="research-table">
                  <table className="data-table">
                    <thead>
                      <tr>
                        <th>交易日</th>
                        <th>模拟成交时间</th>
                        <th>方向</th>
                        <th>手数</th>
                        <th>价格</th>
                        <th>费用</th>
                        <th>计费依据</th>
                        <th>原因</th>
                      </tr>
                    </thead>
                    <tbody>
                      {detail.output.fills.map((f, i) => (
                        <tr key={i}>
                          <td>{f.day}</td>
                          <td>{f.time}</td>
                          <td>{f.side === "BUY" ? "买入" : "卖出"}</td>
                          <td>{f.lots}</td>
                          <td>{f.price}</td>
                          <td>{f.fee}</td>
                          <td>
                            {f.fee_mode === "per_lot"
                              ? "元/手"
                              : "成交金额比例"}{" "}
                            {f.fee_rate} · {f.rule_start} 起
                          </td>
                          <td>{f.reason}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              </details>
              {detail.output.events.map((e, i) => (
                <p key={i}>
                  {e.day} · {e.reason}
                </p>
              ))}
            </>
          )}
        </article>
      )}
    </section>
  );
}
