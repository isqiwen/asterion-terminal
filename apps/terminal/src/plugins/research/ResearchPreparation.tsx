import type { RequestClient } from "../../api/requests";
import { useEffect, useRef, useState } from "react";

import type { components } from "../../api/schema";
import { type CoverageReport } from "../data/client";
import type { Job } from "../tasks/public";
export type Preparation = components["schemas"]["Preparation"];
const names: Record<string, string> = {
  "futures.daily": "日线",
  "futures.calendar": "交易日历",
  "futures.contracts": "合约资料",
};
const states: Record<string, string> = {
  QUEUED: "排队中",
  RUNNING: "同步中",
  SUCCEEDED: "已发布",
  FAILED: "失败",
  CANCELLED: "已取消",
};

export function ResearchPreparation({
  api,
  connected,
  recent,
  onSubmitted,
  onInspect,
}: {
  api: RequestClient;
  connected: boolean;
  recent: Preparation | null;
  onSubmitted: (job: Job) => void;
  onInspect: (versionId: string, reportId: string) => void;
}) {
  const [items, setItems] = useState<Preparation[]>([]);
  const [error, setError] = useState("");
  const [readError, setReadError] = useState("");
  const [ready, setReady] = useState(false);
  const [busy, setBusy] = useState(false);
  const [refreshKey, setRefreshKey] = useState(0);
  const commands = useRef(new Map<string, string>());
  const mutation = useRef(0);
  const writing = useRef(false);
  const live = useRef(true);
  useEffect(() => {
    live.current = true;
    return () => {
      live.current = false;
    };
  }, []);
  useEffect(() => {
    if (!connected) return;
    let active = true,
      reading = false;
    setReady(false);
    async function refresh() {
      if (reading || writing.current) return;
      reading = true;
      const revision = mutation.current;
      try {
        const values = await api.request<Preparation[]>("/data/preparations");
        if (!Array.isArray(values)) throw new Error("准备记录响应无效");
        if (active && revision === mutation.current) {
          setItems(values);
          setReadError("");
          setReady(true);
        }
      } catch (e) {
        if (active && revision === mutation.current) {
          setReadError(String(e));
          setReady(false);
        }
      } finally {
        reading = false;
      }
    }
    void refresh();
    const timer = setInterval(refresh, 2500);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [api, connected, recent?.id, refreshKey]);
  async function retry(job: Job) {
    writing.current = true;
    mutation.current++;
    setBusy(true);
    setError("");
    if (!commands.current.has(job.id))
      commands.current.set(job.id, crypto.randomUUID());
    try {
      const next = await api.request<Job>(`/data/jobs/${job.id}/retry`, {
        command_id: commands.current.get(job.id),
        resume: true,
      });
      if (live.current) onSubmitted(next);
    } catch (e) {
      if (live.current) setError(String(e));
    } finally {
      writing.current = false;
      if (live.current) {
        setBusy(false);
        setRefreshKey((v) => v + 1);
      }
    }
  }
  async function inspect(item: Preparation) {
    setBusy(true);
    setError("");
    const version = (type: string) =>
      item.tasks.find((t) => t.type_id === type)!.job.result!
        .version_id as string;
    try {
      const daily = version("futures.daily");
      const report = await api.request<CoverageReport>(
        `/data/versions/${daily}/coverage`,
        {
          start: item.request.start,
          end: item.request.end,
          use_latest_daily: false,
          calendar_version_id: version("futures.calendar"),
          contracts_version_id: version("futures.contracts"),
        },
      );
      if (live.current) onInspect(daily, report.id);
    } catch (e) {
      if (live.current) setError(String(e));
    } finally {
      if (live.current) setBusy(false);
    }
  }
  const displayed =
    recent && !items.some((i) => i.id === recent.id)
      ? [recent, ...items]
      : items;
  if (!displayed.length && !readError) return null;
  return (
    <section className="research-preparations" aria-label="研究数据准备记录">
      <div className="panel-heading">
        <h2>研究数据准备</h2>
        <small>最近 10 批 · 重开后可继续查看</small>
      </div>
      <p className="panel-footnote">
        先采集合约资料和日历，固定合约身份后自动采集日线。失败不会撤销已发布数据；全部完成后按本批版本核对覆盖，不会自动回测。
      </p>
      {displayed.map((item) => {
        const complete =
          !item.truncated &&
          item.tasks.length === 3 &&
          Object.keys(names).every(
            (type) =>
              item.tasks.filter(
                (t) =>
                  t.type_id === type &&
                  t.job.state === "SUCCEEDED" &&
                  typeof t.job.result?.version_id === "string",
              ).length === 1,
          );
        return (
          <div className="preparation-batch" key={item.id}>
            <p>
              {item.connection_name} · {item.request.exchange} ·{" "}
              {item.request.symbol} · {item.request.start}—{item.request.end} ·{" "}
              {new Date(item.created_at * 1000).toLocaleString()}
            </p>
            {item.daily_state === "WAITING_REFERENCE" && <p role="status">日线等待合约资料通过身份校验</p>}
            {item.daily_state === "IDENTITY_REJECTED" && <p role="alert">{item.identity_error}</p>}
            {item.identity && <p>固定身份：{item.identity.catalog.contracts.map(contract => contract.id).join("、")}</p>}
            <table className="data-table">
              <thead>
                <tr>
                  <th>数据</th>
                  <th>状态</th>
                  <th>进度 / 原因</th>
                  <th>操作</th>
                </tr>
              </thead>
              <tbody>
                {item.tasks.map(({ type_id, job }) => (
                  <tr key={job.id}>
                    <td>{names[type_id] || type_id}</td>
                    <td>{states[job.state]}</td>
                    <td>
                      {job.error ||
                        (job.result?.total
                          ? `${job.result.completed ?? 0} / ${job.result.total} 段`
                          : "—")}
                    </td>
                    <td>
                      {["FAILED", "CANCELLED"].includes(job.state) && (
                        <button
                          disabled={
                            busy || !connected || !ready || item.truncated
                          }
                          onClick={() => void retry(job)}
                        >
                          继续同步{names[type_id] || type_id}
                        </button>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
            {item.truncated && (
              <p role="alert">任务尝试超过展示上限，请到任务中心核查。</p>
            )}
            <button
              disabled={!complete || busy || !connected || !ready}
              onClick={() => void inspect(item)}
            >
              核对覆盖并查看数据
            </button>
          </div>
        );
      })}
      {readError && (
        <p role="alert">准备记录读取失败，暂不能重试或核对：{readError}</p>
      )}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
