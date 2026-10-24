import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
import { tableNames } from "./table-names";
const Panel = lazy(() => import("./DataPanel").then(module => ({ default: module.DataPanel })));
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.data-workbench",
  commands: [
    "task.page",
    "data.download.budget.configure",
    "node.data.local.open",
    "market.observation.preview",
    "data.observation.admit",
    "data.observation.execute",
    "data.observation.read",
    "data.material.sources",
    "data.material.submit",
    "data.material.task",
    "data.material.read",
    "task.action",
    "task.retirement.preview",
    "task.retirement.execute",
    "task.result",
    "data.datasets",
    "data.coverage",
    "data.history.usage",
    "data.history.retirement.preview",
    "data.tables.retirement.preview",
    "data.tables.retirement.execute",
    "data.tables.retirements",
    "data.history.retirement.execute",
    "data.history.retirements",
    "data.download.update.plan",
    "data.download.update.submit",
    "data.download.minutes.submit",
    "data.minutes.page",
    "data.minutes.range",
    "data.minutes.evidence",
    "data.tables.contracts",
    "data.tables.products",
    "data.tables.update.submit",
    "task.result.read",
    "data.tables.refresh.plan",
    "data.tables.refresh.submit",
    "data.tables.coverage",
    "data.tables.page",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  tasks: context =>
    (context.snapshot?.data?.tasks ?? [])
      .filter(
        task =>
          task.kind === "minute_download" ||
          task.kind === "table_update" ||
          task.kind === "material_acquisition",
      )
      .map(task => ({
        id: `data:${context.snapshot?.data?.node_id}:${context.snapshot?.data?.service}:${task.id}`,
        title:
          task.kind === "table_update"
            ? `${t("全市场数据")} · ${task.instrument
                .split(" + ")
                .map(table => (tableNames[table] ? t(tableNames[table]) : table))
                .join(" + ")}`
            : `${task.source_name} · ${task.instrument}`,
        status: t(
          {
            queued: "排队中",
            running: task.kind === "material_acquisition" ? "取得中" : "下载中",
            cancel_requested: "正在取消",
            publishing: "正在发布",
            succeeded: "已完成",
            failed: "失败",
            cancelled: "已取消",
            interrupted: "已中断",
          }[task.state],
        ),
        completed: task.completed,
        total: task.total,
        open: () =>
          context.navigate(
            task.kind === "material_acquisition"
              ? "workspace.materials"
              : task.kind === "table_update"
                ? "workspace.tables"
                : "workspace.history",
            task.kind === "material_acquisition" ? { params: { id: task.id } } : undefined,
          ),
      })),
  workspaces: [
    ["history", "历史数据"],
    ["materials", "情报材料"],
    ["tables", "全市场数据"],
    ["records", "历史数据仓库"],
  ].map(([page, title]) => ({
    id: `workspace.${page}`,
    get title() {
      return t(title);
    },

    component: context => (
      <Panel {...context} workspacePage={page} busy={context.busy || !context.snapshot} />
    ),
  })),
};
