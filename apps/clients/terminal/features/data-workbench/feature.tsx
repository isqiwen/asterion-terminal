import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalContext, TerminalFeature } from "../contract";
import { tableNames } from "./table-names";
const Panel = lazy(() => import("./DataPanel").then(module => ({ default: module.DataPanel })));
const Coverage = lazy(() => import("./Coverage").then(module => ({ default: module.Coverage })));
export const feature: TerminalFeature = {
  id: "asterion.terminal.data-workbench",
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
    ["materials", "资料"],
    ["tables", "全市场数据"],
    ["records", "数据仓库"],
  ]
    .map(([page, title]) => ({
      id: `workspace.${page}`,
      get title() {
        return t(title);
      },
      component: (context: TerminalContext) => (
        <Panel {...context} workspacePage={page} busy={context.busy || !context.snapshot} />
      ),
    }))
    .concat({
      // The Python lab service's warehouse; it does not use the Data service.
      id: "workspace.coverage",
      get title() {
        return t("数据覆盖");
      },
      component: (context: TerminalContext) => <Coverage {...context} />,
    }),
};
