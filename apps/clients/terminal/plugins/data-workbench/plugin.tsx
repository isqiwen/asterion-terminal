import { Icon } from "../contract";
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
  apiVersion: 1,
  commands: [
    "task.page",
    "data.download.budget.configure",
    "node.data_tasks.local.open",
    "task.action",
    "task.result",
    "data.datasets",
    "data.coverage",
    "data.history.usage",
    "data.download.update.plan",
    "data.download.update.submit",
    "data.download.daily.submit",
    "data.daily.page",
    "data.download.minutes.submit",
    "data.minutes.page",
    "data.contracts.load",
    "data.tables.update.submit",
    "data.tables.coverage",
    "data.tables.page",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  tasks: context =>
    (context.snapshot?.task_service?.tasks ?? [])
      .filter(
        task =>
          task.kind === "minute_download" ||
          task.kind === "daily_download" ||
          task.kind === "table_update",
      )
      .map(task => ({
        id: task.id,
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
            running: "下载中",
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
          context.navigate("workspace.data", {
            page: task.kind === "table_update" ? "tables" : "history",
          }),
      })),
  workspace: {
    id: "workspace.data",
    get title() {
      return t("数据");
    },
    icon: <Icon name="data" />,
    component: context => <Panel {...context} busy={context.busy || !context.snapshot} />,
  },
};
