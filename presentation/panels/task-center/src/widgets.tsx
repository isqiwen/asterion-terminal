import { dashboardContribution } from "@asterion/ui-overview/public";
export const widgets = [
  dashboardContribution({ id: "tasks.pulse", title: "异常与提醒", category: "工作", description: "待处理任务与读取异常", width: 1, position: "aside",
    attentionCount: c => c.jobs.filter(j => j.state === "FAILED").length + Number(!!c.catalogError) + Number(!c.connected),
    render: c => <>
      {!c.connected && <p role="alert" className="notice">本机服务未连接，任务状态暂时无法确认。</p>}
      {c.catalogError && <p role="alert" className="notice">数据状态读取失败：{c.catalogError}</p>}
      <ul className="dashboard-list">{c.jobs.filter(j => j.state === "FAILED").map(j => <li key={j.id}><strong className="bad">{c.taskViews.find(v => v.id === j.kind)?.title ?? j.kind} · 失败</strong><p>{j.error || "任务未能完成，请查看任务详情。"}</p></li>)}</ul>
      {!c.jobs.some(j => j.state === "FAILED") && !c.catalogError && c.connected && <p className="dashboard-caption">{c.refreshedAt ? "暂无需要处理的异常" : "正在读取提醒…"}</p>}
      <button onClick={c.openTasks}>打开任务中心 ↗</button>
    </> }),
];
