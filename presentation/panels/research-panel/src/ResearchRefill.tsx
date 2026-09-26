import type { RequestClient } from "@asterion/runtime-client/requests";
import { useEffect, useRef, useState } from "react";

import type { components } from "@asterion/api-types/schema";
import { type CoverageReport, type RefillResult } from "@asterion/ui-data-panel/client";
import type { Job } from "@asterion/ui-task-center/public";
type RefillTracking = components["schemas"]["RefillTracking"];

const labels: Record<string, string> = {
  QUEUED: "排队中",
  RUNNING: "补齐中",
  SUCCEEDED: "已发布",
  FAILED: "失败",
  CANCELLED: "已取消",
};
export type RefillState = {
  jobs: Job[];
  result: RefillResult | null;
  command: string;
  retries: Map<string, string>;
};
export function ResearchRefill({
  report,
  api,
  disabled,
  onSubmitted,
  onInspect,
  onAdopt,
  external,
  cache,
}: {
  cache: Map<string, RefillState>;
  report: CoverageReport;
  api: RequestClient;
  disabled: boolean;
  onSubmitted: (job: Job) => void;
  onInspect: () => void;
  onAdopt: (id: string) => Promise<void>;
  external?: { reportId: string; jobs: Job[] } | null;
}) {
  const saved = cache.get(report.id);
  const [jobs, setJobs] = useState<Job[]>(saved?.jobs || []);
  const [result, setResult] = useState<RefillResult | null>(
    saved?.result || null,
  );
  const [error, setError] = useState("");
  const [statusError, setStatusError] = useState("");
  const [busy, setBusy] = useState(false);
  const [candidate, setCandidate] = useState("");
  const [tracking, setTracking] = useState<RefillTracking | null>(null);
  const [trackingReady, setTrackingReady] = useState(false);
  const [refreshKey, setRefreshKey] = useState(0);
  const mutation = useRef(0);
  const submitting = useRef(false);
  const command = useRef(saved?.command || crypto.randomUUID());
  const retryCommands = useRef(saved?.retries || new Map<string, string>());
  const live = useRef(true);
  useEffect(() => {
    cache.set(report.id, {
      jobs,
      result,
      command: command.current,
      retries: retryCommands.current,
    });
  }, [cache, report.id, jobs, result]);
  useEffect(() => {
    live.current = true;
    return () => {
      live.current = false;
    };
  }, []);
  useEffect(() => {
    if (external?.reportId === report.id)
      setJobs((previous) => [
        ...previous,
        ...external.jobs.filter((j) => !previous.some((p) => p.id === j.id)),
      ]);
  }, [external, report.id]);
  useEffect(() => {
    if (disabled || report.refill_supported === false) return;
    setTrackingReady(false);
    let active = true;
    let reading = false;
    const refresh = async () => {
      if (reading || submitting.current) return;
      reading = true;
      const revision = mutation.current;
      try {
        const value = await api.request<RefillTracking>(
          `/data/coverage/${report.id}/refill-status`,
        );
        if (
          value.report_id !== report.id ||
          !Array.isArray(value.jobs) ||
          !Array.isArray(value.statuses)
        )
          throw new Error("补齐记录响应与当前报告不匹配");
        if (active && revision === mutation.current) {
          setJobs(value.jobs);
          setTracking(value);
          setTrackingReady(true);
          setStatusError("");
        }
      } catch (e) {
        if (active && revision === mutation.current) {
          setTrackingReady(false);
          setStatusError(
            `补齐记录读取失败，暂不能提交或采用新版本：${String(e)}`,
          );
        }
      } finally {
        reading = false;
      }
    };
    void refresh();
    const timer = setInterval(refresh, 2000);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [report.id, report.refill_supported, api, disabled, refreshKey]);
  function beginMutation() {
    submitting.current = true;
    mutation.current++;
    setBusy(true);
    setError("");
  }
  function endMutation() {
    submitting.current = false;
    if (live.current) {
      setBusy(false);
      setRefreshKey((value) => value + 1);
    }
  }
  async function refill() {
    beginMutation();
    try {
      const value = await api.request<RefillResult>(
        `/data/coverage/${report.id}/refill`,
        { command_id: command.current },
        60000,
      );
      if (!live.current) return;
      setResult(value);
      setJobs(value.jobs);
      value.jobs.forEach(onSubmitted);
    } catch (e) {
      if (live.current) setError(String(e));
    } finally {
      endMutation();
    }
  }
  async function retry(job: Job) {
    beginMutation();
    if (!retryCommands.current.has(job.id))
      retryCommands.current.set(job.id, crypto.randomUUID());
    try {
      const value = await api.request<Job>(`/data/jobs/${job.id}/retry`, {
        command_id: retryCommands.current.get(job.id),
        resume: true,
      });
      if (!live.current) return;
      setJobs((previous) => previous.map((p) => (p.id === job.id ? value : p)));
      onSubmitted(value);
    } catch (e) {
      if (live.current) setError(String(e));
    } finally {
      endMutation();
    }
  }
  async function findLatest() {
    setBusy(true);
    setError("");
    setCandidate("");
    try {
      // Re-evaluate against latest only to discover a candidate; never bind it to the draft.
      const checked = await api.request<CoverageReport>(
        `/data/versions/${report.daily_version_id}/coverage`,
        {
          start: report.start,
          end: report.end,
          use_latest_daily: true,
          calendar_version_id: report.calendar_version_id,
          contracts_version_id: report.contracts_version_id,
          reference_symbol: report.reference_symbol,
        },
      );
      if (live.current) {
        setCandidate(checked.daily_version_id);
        if (checked.daily_version_id === report.daily_version_id)
          setError("暂无更新的数据版本；可查看失败原因或到数据集处理。");
      }
    } catch (e) {
      if (live.current) setError(String(e));
    } finally {
      if (live.current) setBusy(false);
    }
  }
  if (report.refill_supported === false)
    return (
      <section aria-label="研究缺口处理">
        <button type="button" onClick={onInspect}>
          查看对应数据与报告
        </button>
        <p>
          文件数据缺口需修正后重新导入，再选择新版本核对。不会自动跨来源补齐。
        </p>
      </section>
    );
  return (
    <section aria-label="研究缺口处理">
      <button type="button" onClick={onInspect}>
        查看对应数据与报告
      </button>
      <button
        type="button"
        disabled={
          disabled ||
          busy ||
          !trackingReady ||
          tracking?.submitted ||
          !!result ||
          jobs.length > 0 ||
          !report.refill_ranges.length ||
          report.status === "CONFLICT"
        }
        onClick={() => void refill()}
      >
        补齐确认缺口
      </button>
      {(result?.status === "NO_GAPS" ||
        tracking?.statuses.includes("NO_GAPS")) && (
        <p>服务端复核后已无可补缺口，请检查新版本。</p>
      )}
      {(result?.status === "BLOCKED" ||
        tracking?.statuses.includes("BLOCKED")) && (
        <p>最新依据存在冲突，补齐被阻止；请到数据集处理。</p>
      )}
      {jobs.length > 0 && (
        <table className="data-table">
          <thead>
            <tr>
              <th>补齐任务</th>
              <th>状态</th>
              <th>进度 / 原因</th>
              <th>操作</th>
            </tr>
          </thead>
          <tbody>
            {jobs.map((j) => (
              <tr key={j.id}>
                <td>{j.id.slice(0, 8)}</td>
                <td>{labels[j.state]}</td>
                <td>
                  {j.error ||
                    (j.result?.total
                      ? `${j.result.completed ?? 0} / ${j.result.total} 段`
                      : "—")}
                </td>
                <td>
                  {["FAILED", "CANCELLED"].includes(j.state) && (
                    <button
                      type="button"
                      disabled={
                        disabled ||
                        busy ||
                        !trackingReady ||
                        tracking?.truncated
                      }
                      onClick={() => void retry(j)}
                    >
                      继续补齐
                    </button>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {jobs.some((j) => j.state === "SUCCEEDED") && (
        <p>已有补齐数据发布。当前回测仍使用原版本；请确认新版本后重新核对。</p>
      )}
      {(result || tracking?.submitted || jobs.length > 0) && (
        <button
          type="button"
          disabled={
            disabled ||
            busy ||
            !trackingReady ||
            tracking?.truncated ||
            jobs.some((j) => ["QUEUED", "RUNNING"].includes(j.state))
          }
          onClick={() => void findLatest()}
        >
          检查补齐后的版本
        </button>
      )}
      {candidate && candidate !== report.daily_version_id && (
        <p>
          当前版本 {report.daily_version_id.slice(0, 8)} → 候选版本{" "}
          {candidate.slice(0, 8)}{" "}
          <button
            type="button"
            disabled={
              disabled ||
              busy ||
              !trackingReady ||
              tracking?.truncated ||
              jobs.some((j) => ["QUEUED", "RUNNING"].includes(j.state))
            }
            onClick={() => {
              setBusy(true);
              void onAdopt(candidate)
                .catch((e) => {
                  if (live.current) setError(String(e));
                })
                .finally(() => {
                  if (live.current) setBusy(false);
                });
            }}
          >
            采用此版本并重新核对
          </button>
        </p>
      )}
      {!trackingReady && !statusError && (
        <p role="status">正在恢复补齐任务记录…</p>
      )}
      {tracking?.submitted && (
        <p>已恢复此报告的补齐记录；状态来自后台，重新打开不会自动提交任务。</p>
      )}
      {tracking?.truncated && (
        <p role="alert">
          此报告超过 200
          次任务记录，仅展示最近记录。请到任务中心核查，当前暂停继续补齐和采用新版本。
        </p>
      )}
      {statusError && <p role="alert">{statusError}</p>}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
