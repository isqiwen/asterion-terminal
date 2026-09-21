import type { RequestClient } from "../../api/requests";
import { useEffect, useRef, useState } from "react";

import type { Job } from "../tasks/public";
import { type CoverageReport, type RefillResult } from "./client";
import { CoverageReferences } from "./CoverageReferences";

const labels: Record<string, string> = {
  COVERED: "所选依据下记录齐全",
  GAPS: "存在待补齐日期",
  UNCONFIRMED: "部分日期无法确认",
  NO_EXPECTED_ROWS: "区间内无预期行情",
  CONFLICT: "行情与核对依据冲突",
  PRESENT: "已有记录",
  GAP: "待补齐",
  CLOSED: "休市",
  OUTSIDE_LISTING: "上市区间外",
  UNKNOWN_CONTRACT: "合约依据不足",
  UNKNOWN_CALENDAR: "日历依据不足",
  PENDING: "当日尚未结束",
};

export function DailyCoveragePanel({
  versionId,
  local = false,
  api,
  first,
  last,
  onInspect,
  onRefill,
  initialReportId,
}: {
  versionId: string;
  local?: boolean;
  api: RequestClient;
  first: string;
  last: string;
  onInspect: (id: string) => void;
  onRefill?: (jobs: Job[], reportId?: string) => void;
  initialReportId?: string;
}) {
  const [start, setStart] = useState(first);
  const [end, setEnd] = useState(last);
  const [referenceSymbol, setReferenceSymbol] = useState("");
  const [note, setNote] = useState("");
  const [calendarId, setCalendarId] = useState("");
  const [contractsId, setContractsId] = useState("");
  const [report, setReport] = useState<CoverageReport | null>(null);
  const [busy, setBusy] = useState("load");
  const [error, setError] = useState("");
  const [result, setResult] = useState<RefillResult | null>(null);
  const [offset, setOffset] = useState(0);
  const [filter, setFilter] = useState("");
  const fillCommand = useRef<{ reportId: string; id: string } | null>(null);
  const sequence = useRef(0);
  useEffect(() => {
    let live = true;
    void api
      .request<CoverageReport | null>(
        initialReportId
          ? `/data/coverage/${initialReportId}`
          : `/data/versions/${versionId}/coverage`,
      )
      .then((value) => {
        if (live && value?.id && value.daily_version_id === versionId) {
          setReport(value);
          if (local) {
            setCalendarId(value.calendar_version_id || "");
            setContractsId(value.contracts_version_id || "");
            setNote(value.reference_note || "");
            setReferenceSymbol(value.reference_symbol || "");
          }
          setStart(value.start);
          setEnd(value.end);
        }
      })
      .catch((reason) => {
        if (live) setError(String(reason));
      })
      .finally(() => {
        if (live) setBusy("");
      });
    return () => {
      live = false;
      sequence.current++;
    };
  }, [versionId, api, initialReportId]);

  function clearReport() {
    setReport(null);
    setResult(null);
    setOffset(0);
    setFilter("");
    setError("");
  }
  async function check(latest: boolean) {
    const current = ++sequence.current;
    setBusy("check");
    setError("");
    setResult(null);
    setReport(null);
    try {
      const value = await api.request<CoverageReport>(
        `/data/versions/${versionId}/coverage`,
        {
          start,
          end,
          use_latest_daily: latest,
          ...(local
            ? {
                reference_policy: "explicit_external",
                reference_note: note,
                reference_symbol: referenceSymbol.trim(),
              }
            : {}),
          ...(calendarId.trim()
            ? { calendar_version_id: calendarId.trim() }
            : {}),
          ...(contractsId.trim()
            ? { contracts_version_id: contractsId.trim() }
            : {}),
        },
      );
      if (current === sequence.current) {
        setReport(value);
        setOffset(0);
        setFilter("");
      }
    } catch (reason) {
      if (current === sequence.current) setError(String(reason));
    } finally {
      if (current === sequence.current) setBusy("");
    }
  }
  async function refill() {
    if (!report) return;
    const current = ++sequence.current;
    if (fillCommand.current?.reportId !== report.id)
      fillCommand.current = { reportId: report.id, id: crypto.randomUUID() };
    setBusy("fill");
    setError("");
    try {
      const value = await api.request<RefillResult>(
        `/data/coverage/${report.id}/refill`,
        { command_id: fillCommand.current.id },
        60000,
      );
      if (current === sequence.current) {
        setResult(value);
        if (value.jobs.length) onRefill?.(value.jobs, report.id);
      }
    } catch (reason) {
      if (current === sequence.current) setError(String(reason));
    } finally {
      if (current === sequence.current) setBusy("");
    }
  }
  const visible =
    report?.days.filter((day) => !filter || day.status === filter) ?? [];
  return (
    <details
      className="daily-coverage"
      open={initialReportId ? true : undefined}
    >
      <summary>日线覆盖核对与补齐</summary>
      <div className="daily-coverage-body">
        <form
          className="coverage-form"
          onSubmit={(event) => {
            event.preventDefault();
            void check(false);
          }}
        >
          <label>
            核对开始日期
            <input
              type="date"
              aria-label="核对开始日期"
              required
              value={start}
              disabled={!!busy}
              onChange={(e) => {
                setStart(e.target.value);
                clearReport();
              }}
            />
          </label>
          <label>
            核对结束日期
            <input
              type="date"
              aria-label="核对结束日期"
              required
              value={end}
              disabled={!!busy}
              onChange={(e) => {
                setEnd(e.target.value);
                clearReport();
              }}
            />
          </label>
          <button
            disabled={
              !!busy ||
              (local &&
                (!calendarId.trim() ||
                  !contractsId.trim() ||
                  !note.trim() ||
                  !referenceSymbol.trim()))
            }
          >
            核对选中版本
          </button>
          <button
            type="button"
            disabled={local || !!busy || !start || !end}
            onClick={() => void check(true)}
          >
            核对最新行情版本
          </button>
        </form>
        {local ? (
          <CoverageReferences
            api={api}
            calendar={calendarId}
            contracts={contractsId}
            note={note}
            symbol={referenceSymbol}
            disabled={!!busy}
            onChange={(c, r, n, s) => {
              setReferenceSymbol(s);
              setCalendarId(c);
              setContractsId(r);
              setNote(n);
              clearReport();
            }}
          />
        ) : (
          <details className="coverage-references">
            <summary>指定核对依据（留空使用同源、同交易所的最新版本）</summary>
            <div className="coverage-form">
              <label>
                日历版本
                <input
                  aria-label="核对日历版本"
                  value={calendarId}
                  disabled={!!busy}
                  onChange={(e) => {
                    setCalendarId(e.target.value);
                    clearReport();
                  }}
                />
              </label>
              <label>
                合约资料版本
                <input
                  aria-label="核对合约资料版本"
                  value={contractsId}
                  disabled={!!busy}
                  onChange={(e) => {
                    setContractsId(e.target.value);
                    clearReport();
                  }}
                />
              </label>
            </div>
          </details>
        )}
        {busy && (
          <p role="status">
            {busy === "fill"
              ? "正在复核缺口并提交任务…"
              : busy === "check"
                ? "正在核对固定版本…"
                : "正在读取已有覆盖报告…"}
          </p>
        )}
        {error && (
          <p role="alert" className="error">
            {error}
          </p>
        )}
        {report && (
          <>
            <p className="coverage-status" role="status">
              {labels[report.status]} · {report.start} — {report.end}
            </p>
            <div className="coverage-form">
              <button onClick={() => onInspect(report.daily_version_id)}>
                查看核对行情版本
              </button>
              {report.calendar_version_id ? (
                <button onClick={() => onInspect(report.calendar_version_id!)}>
                  查看核对日历版本
                </button>
              ) : (
                <span>缺少同源交易日历</span>
              )}
              {report.contracts_version_id ? (
                <button onClick={() => onInspect(report.contracts_version_id!)}>
                  查看核对合约版本
                </button>
              ) : (
                <span>缺少同源合约资料</span>
              )}
            </div>
            <p>
              {Object.entries(report.counts)
                .map(
                  ([status, count]) =>
                    `${labels[status] ?? status} ${count} 天`,
                )
                .join(" · ")}
            </p>
            <p>身份目录：{report.identity?.catalog_id || "未确认"}</p>
            {report.identity && <p>实际合约：{report.identity.catalog.contracts.map((c) => c.id).join("、")}</p>}
            {report.reference_note && <p>关联说明：{report.reference_note}</p>}
            {report.notes.map((note) => (
              <p className="muted" key={note}>
                {note}
              </p>
            ))}
            <div className="coverage-form">
              <button
                disabled={
                  report.refill_supported === false ||
                  !!busy ||
                  !report.refill_ranges.length ||
                  !!result
                }
                onClick={() => void refill()}
              >
                补齐确认缺口（{report.refill_ranges.length} 个区间）
              </button>
              <span>只补取有依据的历史缺口；提交前跳过已补上的日期。</span>
            </div>
            {result && (
              <p role="status">
                {result.status === "QUEUED"
                  ? `已提交 ${result.jobs.length} 项补齐任务，跳过 ${result.skipped_gap_days} 个已补齐日期。任务结束后请重新核对最新行情版本。`
                  : result.status === "BLOCKED"
                    ? "最新数据与依据冲突，未创建补齐任务。"
                    : "没有仍需补取的确认缺口，未创建任务。"}
              </p>
            )}
            <div className="coverage-form">
              <label>
                逐日状态
                <select
                  aria-label="覆盖日期状态"
                  value={filter}
                  onChange={(e) => {
                    setFilter(e.target.value);
                    setOffset(0);
                  }}
                >
                  <option value="">全部日期</option>
                  {Object.keys(report.counts).map((status) => (
                    <option value={status} key={status}>
                      {labels[status] ?? status}
                    </option>
                  ))}
                </select>
              </label>
              <button
                disabled={offset === 0}
                onClick={() => setOffset((value) => value - 50)}
              >
                上一页核对日期
              </button>
              <span>
                共 {visible.length} 天 · 第 {Math.floor(offset / 50) + 1} 页
              </span>
              <button
                disabled={offset + 50 >= visible.length}
                onClick={() => setOffset((value) => value + 50)}
              >
                下一页核对日期
              </button>
            </div>
            <table className="data-table" aria-label="日线覆盖逐日结果">
              <thead>
                <tr>
                  <th>日期</th>
                  <th>核对状态</th>
                  <th>已存行情</th>
                  <th>依据说明</th>
                </tr>
              </thead>
              <tbody>
                {visible.slice(offset, offset + 50).map((day) => (
                  <tr key={day.date}>
                    <td>{day.date}</td>
                    <td>{labels[day.status]}</td>
                    <td>{day.has_data ? "有" : "无"}</td>
                    <td>{day.reason}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <p className="muted mono">
              报告 {report.id} · 核对日期 {report.as_of}
            </p>
          </>
        )}
      </div>
    </details>
  );
}
