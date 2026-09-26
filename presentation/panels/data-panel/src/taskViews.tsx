import type { Job, TaskContext, TaskView } from "@asterion/ui-task-center/public";
import { IngestionDetails } from "./IngestionDetails";

function Retry({ job, api, onSubmitted, onError }: TaskContext) {
  if (!["FAILED", "CANCELLED"].includes(job.state)) return null;
  const retry = (resume: boolean) =>
    void api
      .request<Job>(`/data/jobs/${job.id}/retry`, {
        resume,
        command_id: crypto.randomUUID(),
      })
      .then(onSubmitted)
      .catch((error) => onError(String(error)));
  return (
    <>
      <button
        title="复用有效分段，仅重新采集失败或缺失部分"
        onClick={() => retry(true)}
      >
        继续同步
      </button>
      <button title="重新获取整个请求范围的数据" onClick={() => retry(false)}>
        全部重新同步
      </button>
    </>
  );
}
export const taskViews: readonly TaskView[] = [
  {
    id: "data.sync",
    title: "数据源同步",
    result: (job) => (job.state === "SUCCEEDED" ? "数据集已发布" : "—"),
    detail: {
      title: "采集详情",
      render: (context, onBack) => (
        <IngestionDetails job={context.job} api={context.api} onBack={onBack} />
      ),
    },
    actions: (context) => <Retry {...context} />,
  },
  {
    id: "data.import_csv",
    title: "历史行情导入",
    result: (job) =>
      job.result?.dataset_id
        ? "数据集已发布"
        : job.result?.snapshot_id
          ? "快照已发布"
          : "—",
  },
];
