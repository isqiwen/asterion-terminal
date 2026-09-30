import { Icon } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
const Panel = lazy(() => import("./DataPanel").then(module => ({ default: module.DataPanel })));
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.data-workbench",
  apiVersion: 1,
  commands: [
    "research.local",
    "research.action",
    "research.result",
    "research.datasets",
    "research.daily.submit",
    "research.daily.page",
    "research.minutes.submit",
    "research.minutes.page",
    "research.contracts.load",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  tasks: context =>
    (context.snapshot?.research?.tasks ?? [])
      .filter(
        task =>
          task.kind === "data_import" ||
          task.kind === "calendar_import" ||
          task.kind === "minute_download" ||
          task.kind === "daily_download",
      )
      .map(task => ({
        id: task.id,
        title: `${task.source_name} · ${task.instrument}`,
        status: t(
          {
            queued: "排队中",
            running:
              task.kind === "minute_download" || task.kind === "daily_download"
                ? "下载中"
                : "发布中",
            cancel_requested: "正在取消",
            succeeded:
              task.kind === "minute_download" || task.kind === "daily_download"
                ? "已完成"
                : "已发布",
            failed: "失败",
            cancelled: "已取消",
            interrupted: "已中断",
          }[task.state],
        ),
        completed: task.completed,
        total: task.total,
        open: () =>
          context.navigate("workspace.data", {
            page:
              task.kind === "minute_download" || task.kind === "daily_download"
                ? "history"
                : "records",
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
  widgets: context => {
    const data = context.snapshot?.dataset;
    const openData = () => context.navigate("workspace.data", { page: "history" });
    return [
      {
        id: "data.local",
        title: t("数据工作台"),
        category: t("数据"),
        description: t("从数据源下载历史数据"),
        width: 2,
        column: "primary",
        defaultVisible: true,
        hasContent: !!data,
        render: () => (
          <div className="overview-data">
            <strong>{data ? data.filename : t("尚未选择历史数据")}</strong>
            <p>
              {data
                ? t(data.persistent ? "已加载发布版本" : "已完成逐笔校验 · 仅当前会话预览")
                : t("选择数据源和具体月份合约，下载完整历史数据。")}
            </p>
            <button onClick={openData}>{t("打开数据工作区")}</button>
          </div>
        ),
      },
    ];
  },
};
