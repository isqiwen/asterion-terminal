import { Icon } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
const Workspace = lazy(() => import("./Workspace").then(module => ({ default: module.Workspace })));
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.research",
  apiVersion: 1,
  commands: [
    "task.page",
    "node.data_tasks.local.open",
    "data.datasets",
    "data.dataset.saved",
    "data.dataset.save",
    "data.dataset.use",
    "data.dataset.select",
    "data.dataset.series",
    "data.dataset.remove",
    "backtest.submit",
    "factor.submit",
    "factor.daily.submit",
    "task.action",
    "task.result",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  tasks: context =>
    (context.snapshot?.task_service?.tasks ?? [])
      .filter(
        task => task.kind === "backtest" || task.kind === "factor" || task.kind === "daily_factor",
      )
      .map(task => ({
        id: task.id,
        title: `${task.instrument} · ${task.kind === "daily_factor" ? t("日线因子") : task.kind === "factor" ? t("动量因子") : `${t("均线回测")} · ${task.trading_day}`}`,
        status: t(
          {
            queued: "排队中",
            running: "运行中",
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
          context.navigate("workspace.research", {
            page: "task",
            params: { id: task.id, kind: task.kind },
          }),
      })),
  workspace: {
    id: "workspace.research",
    get title() {
      return t("研究");
    },
    icon: <Icon name="backtest" />,
    component: context => <Workspace {...context} />,
  },
};
