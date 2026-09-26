import type { RequestClient } from "@asterion/runtime-client/requests";
import { useEffect, useRef, useState } from "react";

import { type CoverageReport } from "@asterion/ui-data-panel/client";
import { CoverageReferences } from "@asterion/ui-data-panel/CoverageReferences";

const labels: Record<string, string> = {
  COVERED: "所选依据下记录齐全",
  GAPS: "存在缺失日线",
  UNCONFIRMED: "部分日期无法确认",
  CONFLICT: "行情与核对依据冲突",
  NO_EXPECTED_ROWS: "区间内无预期行情",
  GAP: "缺失日线",
  UNKNOWN_CALENDAR: "日历依据不足",
  UNKNOWN_CONTRACT: "合约依据不足",
  PENDING: "当日尚未结束",
  PRESENT: "已有记录",
  CLOSED: "休市",
  OUTSIDE_LISTING: "上市区间外",
};
export function CoverageEvidence({ report }: { report: CoverageReport }) {
  const [page, setPage] = useState(0);
  const issues = report.days.filter(
    (d) => !["PRESENT", "CLOSED", "OUTSIDE_LISTING"].includes(d.status),
  );
  return (
    <section aria-label="覆盖依据">
      <p>
        <strong>{labels[report.status]}</strong> · {report.start}—{report.end} ·
        已有 {report.counts.PRESENT || 0} 日 · 缺失 {report.counts.GAP || 0} 日
        · 待确认{" "}
        {(report.counts.UNKNOWN_CALENDAR || 0) +
          (report.counts.UNKNOWN_CONTRACT || 0) +
          (report.counts.PENDING || 0)}{" "}
        日
      </p>
      <details>
        <summary>核对依据与异常日期</summary>
        <p className="research-hash">
          报告：{report.id}
          <br />
          日线：{report.daily_version_id}
          <br />
          日历：{report.calendar_version_id || "缺失"}
          <br />
          合约资料：{report.contracts_version_id || "缺失"}
          <br />
          目录：{report.identity?.catalog_id || "缺失"}
          <br />
          实际合约：
          {report.identity?.catalog.contracts.map((c) => c.id).join("、") ||
            "未确认"}
          <br />
          核对日期：{report.as_of}
          {report.reference_policy === "explicit_external" && (
            <>
              <br />
              关联说明：{report.reference_note}
              <br />
              依据来源：
              {Object.entries(report.references || {})
                .map(
                  ([key, value]) =>
                    `${key}: ${(value as { source?: string }).source || "—"}`,
                )
                .join(" · ")}
            </>
          )}
        </p>
        {report.notes.map((n) => (
          <p key={n}>{n}</p>
        ))}
        {issues.length > 0 && (
          <>
            <table className="data-table">
              <thead>
                <tr>
                  <th>日期</th>
                  <th>状态</th>
                  <th>原因</th>
                </tr>
              </thead>
              <tbody>
                {issues.slice(page * 50, (page + 1) * 50).map((d) => (
                  <tr key={d.date}>
                    <td>{d.date}</td>
                    <td>{labels[d.status]}</td>
                    <td>{d.reason}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <button
              type="button"
              disabled={page === 0}
              onClick={() => setPage((p) => p - 1)}
            >
              上一页异常日期
            </button>
            <span>
              {" "}
              {page + 1} / {Math.ceil(issues.length / 50)}{" "}
            </span>
            <button
              type="button"
              disabled={(page + 1) * 50 >= issues.length}
              onClick={() => setPage((p) => p + 1)}
            >
              下一页异常日期
            </button>
          </>
        )}
      </details>
    </section>
  );
}
export function ResearchCoverage({
  api,
  versionId,
  start,
  end,
  supported,
  local = false,
  disabled,
  report,
  onReport,
}: {
  api: RequestClient;
  versionId: string;
  start: string;
  end: string;
  supported: boolean;
  local?: boolean;
  disabled: boolean;
  report: CoverageReport | null;
  onReport: (value: CoverageReport | null) => void;
}) {
  const [calendar, setCalendar] = useState(report?.calendar_version_id || "");
  const [contracts, setContracts] = useState(
    report?.contracts_version_id || "",
  );
  const [referenceSymbol, setReferenceSymbol] = useState(
    report?.reference_symbol || "",
  );
  const [note, setNote] = useState(report?.reference_note || "");
  const live = useRef(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => {
    live.current = true;
    return () => {
      live.current = false;
    };
  }, []);
  async function check() {
    setBusy(true);
    setError("");
    onReport(null);
    try {
      const value = await api.request<CoverageReport>(
        `/data/versions/${versionId}/coverage`,
        {
          start,
          end,
          use_latest_daily: false,
          ...(local
            ? {
                reference_policy: "explicit_external",
                reference_note: note,
                reference_symbol: referenceSymbol.trim(),
                calendar_version_id: calendar.trim(),
                contracts_version_id: contracts.trim(),
              }
            : {}),
        },
      );
      if (live.current) onReport(value);
    } catch (e) {
      if (live.current) setError(String(e));
    } finally {
      if (live.current) setBusy(false);
    }
  }
  return (
    <section aria-label="回测前覆盖核对">
      {local && (
        <CoverageReferences
          api={api}
          calendar={calendar}
          contracts={contracts}
          note={note}
          symbol={referenceSymbol}
          disabled={disabled || busy}
          onChange={(c, r, n, s) => {
            setReferenceSymbol(s);
            setCalendar(c);
            setContracts(r);
            setNote(n);
            onReport(null);
          }}
        />
      )}
      <button
        type="button"
        disabled={
          disabled ||
          busy ||
          !supported ||
          !versionId ||
          !start ||
          !end ||
          (local &&
            (!calendar.trim() ||
              !contracts.trim() ||
              !note.trim() ||
              !referenceSymbol.trim()))
        }
        onClick={() => void check()}
      >
        {busy ? "核对中…" : "核对所选版本覆盖"}
      </button>
      {!supported && versionId && (
        <p>
          该版本暂不支持同源覆盖核对。请改选可核对的固定日线版本；探索模式也需要合约身份依据。
        </p>
      )}
      {error && <p role="alert">{error}</p>}
      {report ? (
        <CoverageEvidence key={report.id} report={report} />
      ) : (
        <p>尚无匹配当前版本和日期范围的报告。</p>
      )}
      <p>
        发现缺口时，请在数据集详情补齐，再选择新版本重新核对；覆盖通过不代表数据在历史时点已知。
      </p>
    </section>
  );
}
