import type { RequestClient } from "../../api/requests";
import { useEffect, useState } from "react";

import type { Job } from "../tasks/public";
import {
  type ObservationPage,
  type ObservationPreview,
  type ObservationRecord,
} from "./client";

const statuses: Record<string, string> = {
  RECEIVED: "已接收 · 待发布校验",
  EMPTY_UNCONFIRMED: "空响应 · 原因未确认",
  AUTH_FAILED: "凭据无效或已失效",
  PERMISSION_DENIED: "接口权限不足",
  RATE_LIMITED: "调用频率或额度受限",
  PROVIDER_REJECTED: "数据源拒绝请求",
  FETCH_FAILED: "采集失败",
};

function range(record: ObservationRecord) {
  const params = record.manifest.partition.params;
  return [
    params.exchange,
    params.ts_code,
    params.start_date && `${params.start_date} – ${params.end_date}`,
  ]
    .filter(Boolean)
    .join(" · ");
}

export function IngestionDetails({
  job,
  api,
  onBack,
}: {
  job: Pick<Job, "id" | "state" | "error">;
  api: RequestClient;
  onBack: () => void;
}) {
  const [offset, setOffset] = useState(0);
  const [page, setPage] = useState<ObservationPage | null>(null);
  const [selected, setSelected] = useState<ObservationRecord | null>(null);
  const [rowOffset, setRowOffset] = useState(0);
  const [preview, setPreview] = useState<ObservationPreview | null>(null);
  const [error, setError] = useState("");
  const [previewError, setPreviewError] = useState("");
  const [revision, setRevision] = useState(0);
  const active = job.state === "RUNNING" || job.state === "QUEUED";

  useEffect(() => {
    const refresh = () => setRevision((value) => value + 1);
    window.addEventListener("asterion:unlocked", refresh);
    return () => window.removeEventListener("asterion:unlocked", refresh);
  }, []);

  useEffect(() => {
    if (!active || selected) return;
    const timer = window.setInterval(() => setRevision((r) => r + 1), 5000);
    return () => window.clearInterval(timer);
  }, [active, selected]);

  useEffect(() => {
    let live = true;
    setPage(null);
    setError("");
    void api
      .request<ObservationPage>(
        `/data/jobs/${job.id}/observations?offset=${offset}&limit=20`,
      )
      .then((value) => {
        if (live) setPage(value);
      })
      .catch((reason) => {
        if (live) setError(String(reason));
      });
    return () => {
      live = false;
    };
  }, [job.id, job.state, api, offset, revision]);

  useEffect(() => {
    let live = true;
    setPreview(null);
    setPreviewError("");
    if (selected)
      void api
        .request<ObservationPreview>(
          `/data/jobs/${job.id}/observations/${selected.attempt}/${selected.partition_index}?offset=${rowOffset}&limit=50`,
        )
        .then((value) => {
          if (live) setPreview(value);
        })
        .catch((reason) => {
          if (live) setPreviewError(String(reason));
        });
    return () => {
      live = false;
    };
  }, [job.id, api, selected, rowOffset, revision]);

  return (
    <div className="ingestion-details panel-scroll" aria-label="采集详情">
      <div className="ingestion-toolbar">
        <button onClick={onBack}>返回任务列表</button>
        <h3>采集详情 · {job.id.slice(0, 8)}</h3>
        <button onClick={() => setRevision((r) => r + 1)}>刷新采集详情</button>
      </div>
      <p>
        {job.state === "SUCCEEDED"
          ? "本任务已通过校验并发布。下方展示各次尝试的原始采集记录。"
          : "已接收仅表示原始响应已保存，不代表校验通过、覆盖完整或已发布。"}
      </p>
      {job.error && (
        <p className="error" role="status">
          任务失败原因：{job.error}
        </p>
      )}
      {selected ? (
        <>
          <div className="ingestion-toolbar">
            <button
              onClick={() => {
                setSelected(null);
                setRowOffset(0);
              }}
            >
              返回分段列表
            </button>
            <h3>
              第 {selected.attempt} 次尝试 · 第 {selected.partition_index + 1}{" "}
              段 · 原始预览
            </h3>
          </div>
          <p>
            {range(selected)} ·{" "}
            {statuses[selected.manifest.status] || selected.manifest.status}
          </p>
          {selected.manifest.reused_from && (
            <p>
              复用自任务 {selected.manifest.reused_from.job_id.slice(0, 8)} · 第{" "}
              {selected.manifest.reused_from.attempt} 次尝试；保留原始采集时间。
            </p>
          )}
          <p className="mono ingestion-checksum">
            校验和：{selected.manifest.checksum}
          </p>
          {previewError ? (
            <p role="alert" className="error">
              {previewError}
            </p>
          ) : !preview ? (
            <p role="status">正在读取原始数据…</p>
          ) : (
            <>
              <div className="ingestion-table-scroll">
                <table className="data-table" aria-label="原始数据">
                  <thead>
                    <tr>
                      {preview.manifest.partition.fields.map((field) => (
                        <th key={field}>{field}</th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {preview.rows.map((row, index) => (
                      <tr key={rowOffset + index}>
                        {preview.manifest.partition.fields.map((field) => (
                          <td key={field}>
                            {row[field] === null || row[field] === undefined
                              ? "—"
                              : typeof row[field] === "object"
                                ? JSON.stringify(row[field])
                                : String(row[field])}
                          </td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
              {!preview.total && (
                <p>此分段没有原始数据行。空响应不能作为休市或无成交的证明。</p>
              )}
              <div className="ingestion-toolbar">
                <button
                  disabled={rowOffset === 0}
                  onClick={() => setRowOffset((v) => v - 50)}
                >
                  上一页原始数据
                </button>
                <span>
                  共 {preview.total} 行 · 第 {Math.floor(rowOffset / 50) + 1} 页
                </span>
                <button
                  disabled={rowOffset + 50 >= preview.total}
                  onClick={() => setRowOffset((v) => v + 50)}
                >
                  下一页原始数据
                </button>
              </div>
            </>
          )}
        </>
      ) : (
        <>
          {error ? (
            <p role="alert" className="error">
              {error}
            </p>
          ) : !page ? (
            <p role="status">正在读取采集记录…</p>
          ) : (
            <>
              <div className="ingestion-table-scroll">
                <table className="data-table" aria-label="采集分段">
                  <thead>
                    <tr>
                      <th>尝试 / 分段</th>
                      <th>请求范围</th>
                      <th>采集状态</th>
                      <th>行数</th>
                      <th>采集时间</th>
                      <th>操作</th>
                    </tr>
                  </thead>
                  <tbody>
                    {page.items.map((record) => (
                      <tr key={`${record.attempt}-${record.partition_index}`}>
                        <td>
                          {record.attempt} / {record.partition_index + 1}
                        </td>
                        <td>{range(record)}</td>
                        <td>
                          {statuses[record.manifest.status] ||
                            record.manifest.status}
                          {record.manifest.reused_from ? " · 已复用" : ""}
                        </td>
                        <td>{record.manifest.rows}</td>
                        <td>
                          {new Date(record.manifest.observed_at).toLocaleString(
                            "zh-CN",
                            { hour12: false },
                          )}
                        </td>
                        <td>
                          <button
                            onClick={() => {
                              setSelected(record);
                              setRowOffset(0);
                            }}
                          >
                            查看原始数据
                          </button>
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
              {!page.total && (
                <p>
                  暂无已保存的分段。
                  {active
                    ? "采集后会自动刷新。"
                    : "该任务尚未保存证据，或创建于证据留存功能启用前。"}
                </p>
              )}
              <div className="ingestion-toolbar">
                <button
                  disabled={offset === 0}
                  onClick={() => setOffset((v) => v - 20)}
                >
                  上一页分段
                </button>
                <span>
                  共 {page.total} 段 · 第 {Math.floor(offset / 20) + 1} 页
                </span>
                <button
                  disabled={offset + 20 >= page.total}
                  onClick={() => setOffset((v) => v + 20)}
                >
                  下一页分段
                </button>
              </div>
            </>
          )}
        </>
      )}
    </div>
  );
}
