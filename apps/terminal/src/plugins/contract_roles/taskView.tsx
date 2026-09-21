import { useState } from "react";
import type { Job, TaskContext, TaskView } from "../tasks/public";

function Retry({ job, api, onSubmitted, onError }: TaskContext) {
  const [busy, setBusy] = useState(false);
  if (job.state !== "FAILED") return null;
  return <button disabled={busy} title="保持原始输入和确认状态，重试失败的续算任务" onClick={() => {
    setBusy(true);
    void api.request<Job>(`/contract-roles/computed/tasks/${job.id}/retry`, {})
      .then(onSubmitted).catch((error) => onError(String(error))).finally(() => setBusy(false));
  }}>{busy ? "提交中…" : "重试续算"}</button>;
}

export const taskView: TaskView = {
  id: "contract_roles.continue",
  title: "合约角色续算",
  result: (job) => job.result?.computed_version_id
    ? <span title={String(job.result.computed_version_id)}>角色已发布 · {String(job.result.computed_version_id).slice(0, 12)}</span>
    : "—",
  actions: (context) => <Retry {...context} />,
};
