import type { RequestClient } from "../../api/requests";
import { useState, type PointerEvent } from "react";
import type { Job, TaskContext, TaskView } from "./public";
const labels: Record<string, string> = {
  QUEUED: "已接收",
  RUNNING: "执行中",
  SUCCEEDED: "已完成",
  FAILED: "失败",
  CANCELLED: "已取消",
};
export function TaskDock({
  jobs,
  api,
  height,
  onResize,
  onClose,
  onCancel,
  views,
  onSubmitted,
  onError,
}: {
  jobs: Job[];
  api: RequestClient;
  height: number;
  onResize: (height: number) => void;
  onClose: () => void;
  onCancel: (id: string) => void;
  views: readonly TaskView[];
  onSubmitted: TaskContext["onSubmitted"];
  onError: TaskContext["onError"];
}) {
  const [detailId, setDetailId] = useState<string | null>(null);
  const detailJob = jobs.find((job) => job.id === detailId);
  function resize(e: PointerEvent<HTMLDivElement>) {
    e.currentTarget.setPointerCapture(e.pointerId);
    const start = e.clientY;
    const element = e.currentTarget;
    const move = (event: globalThis.PointerEvent) =>
      onResize(Math.max(120, Math.min(400, height + start - event.clientY)));
    const end = () => {
      element.removeEventListener("pointermove", move);
      element.removeEventListener("pointerup", end);
      element.removeEventListener("pointercancel", end);
    };
    element.addEventListener("pointermove", move);
    element.addEventListener("pointerup", end);
    element.addEventListener("pointercancel", end);
  }
  return (
    <section className="task-dock" aria-label="任务中心" style={{ height }}>
      <div
        className="dock-resize"
        onPointerDown={resize}
        title="拖动调整任务面板高度"
      />
      <div className="panel-heading">
        <h2>任务中心</h2>
        <span className="count">{jobs.length}</span>
        <span className="panel-spacer" />
        <small>最近 100 项</small>
        <button aria-label="收起任务中心" onClick={onClose}>
          ⌄
        </button>
      </div>
      {detailJob ? (
        views
          .find((view) => view.id === detailJob.kind)
          ?.detail?.render({ job: detailJob, api, onSubmitted, onError }, () =>
            setDetailId(null),
          )
      ) : (
        <div className="panel-scroll">
          <table className="data-table">
            <thead>
              <tr>
                <th>任务</th>
                <th>标识</th>
                <th>状态</th>
                <th>提交时间</th>
                <th className="numeric">尝试</th>
                <th>结果</th>
              </tr>
            </thead>
            <tbody>
              {jobs.map((j) => (
                <tr key={j.id}>
                  <td>
                    {views.find((view) => view.id === j.kind)?.title ?? j.kind}
                  </td>
                  <td className="mono" title={j.id}>
                    {j.id.slice(0, 8)}
                  </td>
                  <td className={`state ${j.state}`}>{labels[j.state]}</td>
                  <td className="mono">
                    {new Date(j.created_at * 1000).toLocaleTimeString("zh-CN", {
                      hour12: false,
                    })}
                  </td>
                  <td className="numeric">{j.attempt}</td>
                  <td>
                    <span>
                      {j.error ||
                        (["RUNNING", "QUEUED"].includes(j.state) ? (
                          <>
                            <span>
                              {j.result?.total
                                ? `${j.result.completed} / ${j.result.total} 段 `
                                : ""}
                            </span>
                            <button onClick={() => onCancel(j.id)}>取消</button>
                          </>
                        ) : (
                          (views
                            .find((view) => view.id === j.kind)
                            ?.result(j) ?? "—")
                        ))}
                    </span>
                    {views.find((view) => view.id === j.kind)?.detail && (
                      <button
                        onClick={() => {
                          setDetailId(j.id);
                          onResize(Math.max(height, 360));
                        }}
                      >
                        {
                          views.find((view) => view.id === j.kind)!.detail!
                            .title
                        }
                      </button>
                    )}
                    {views
                      .find((view) => view.id === j.kind)
                      ?.actions?.({ job: j, api, onSubmitted, onError })}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          {!jobs.length && <div className="table-empty">暂无任务</div>}
        </div>
      )}
    </section>
  );
}
