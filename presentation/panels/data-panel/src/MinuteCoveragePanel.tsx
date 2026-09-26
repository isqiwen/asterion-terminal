import { useState } from "react";
import type { RequestClient } from "@asterion/runtime-client/requests";
import type { components } from "@asterion/api-types/schema";
type Report = components["schemas"]["MinuteCoverage"];

export function MinuteCoveragePanel({
  api,
  versionId,
  days,
}: {
  api: RequestClient;
  versionId: string;
  days: string[];
}) {
  const [day, setDay] = useState(days.at(-1) ?? "");
  const [report, setReport] = useState<Report | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  return (
    <section aria-label="分钟覆盖核对" className="import-options">
      <label>
        核对交易日
        <input
          aria-label="分钟核对交易日"
          type="date"
          value={day}
          disabled={busy}
          onChange={(e) => {
            setDay(e.target.value);
            setReport(null);
          }}
        />
      </label>
      <button
        disabled={busy || !day}
        onClick={async () => {
          setBusy(true);
          setError("");
          setReport(null);
          try {
            setReport(
              await api.request<Report>(
                `/data/versions/${versionId}/minute-coverage?trading_day=${encodeURIComponent(day)}`,
                {},
              ),
            );
          } catch (e) {
            setError(String(e));
          } finally {
            setBusy(false);
          }
        }}
      >
        {busy ? "核对中…" : "核对分钟覆盖"}
      </button>
      {error && <p role="alert">{error}</p>}
      {report && (
        <>
          <p role="status">
            {report.status === "UNVERIFIED"
              ? "完整性待核验"
              : report.status === "COVERED"
                ? "分钟记录齐全"
                : "存在缺失分钟"}{" "}
            · {report.frequency} · 预期 {report.expected ?? "未确定"} · 已有{" "}
            {report.present} · 缺失 {report.missing?.length ?? "未确定"}
          </p>
          <p>{report.note}</p>
          <details>
            <summary>缺失时间与固定依据</summary>
            <p>实际合约：{report.contract_id}</p>
            <p>交易时间版本：{report.trading_time_id}</p>
            <div className="history-contracts">
              {report.missing?.map((stamp) => (
                <div key={stamp}>{stamp}</div>
              ))}
            </div>
          </details>
        </>
      )}
    </section>
  );
}
