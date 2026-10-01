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
    "research.coverage",
    "research.daily.submit",
    "research.daily.page",
    "research.minutes.submit",
    "research.minutes.page",
    "research.contracts.load",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  tasks: context =>
    (context.snapshot?.research?.tasks ?? [])
      .filter(task => task.kind === "minute_download" || task.kind === "daily_download")
      .map(task => ({
        id: task.id,
        title: `${task.source_name} · ${task.instrument}`,
        status: t(
          {
            queued: "排队中",
            running: "下载中",
            cancel_requested: "正在取消",
            succeeded: "已完成",
            failed: "失败",
            cancelled: "已取消",
            interrupted: "已中断",
          }[task.state],
        ),
        completed: task.completed,
        total: task.total,
        open: () => context.navigate("workspace.data", { page: "history" }),
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
    const datasets = context.snapshot?.datasets ?? [];
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
        hasContent: datasets.length > 0,
        render: () => (
          <div className="overview-data">
            <strong>
              {datasets.length
                ? datasets.map(item => `${item.venue} · ${item.symbol}`).join(" + ")
                : t("尚未选择历史数据")}
            </strong>
            <p>
              {datasets.length
                ? t("{count} 根 K 线 · {first} – {last}", {
                    count: datasets.reduce((sum, item) => sum + item.count, 0),
                    first: datasets[0].first_day,
                    last: datasets[0].last_day,
                  })
                : t("选择数据源和具体月份合约，下载完整历史数据。")}
            </p>
            <button onClick={openData}>{t("打开数据工作区")}</button>
          </div>
        ),
      },
    ];
  },
};
