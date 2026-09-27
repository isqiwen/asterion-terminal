import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) => translate("asterion.terminal.research", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
const Panel = lazy(() => import("./Panel").then(module => ({ default: module.Panel })));
export const plugin: TerminalPlugin = {
    id: "asterion.terminal.research", apiVersion: 1, languageResources: { "zh-CN": zh, "en-US": en },
    tasks: context => (context.snapshot?.research?.tasks ?? []).filter(task=>(task.kind==="backtest"||task.kind==="factor")).map(task => ({
        id: task.id, title: `${task.instrument} · ${task.kind === "factor" ? t("动量因子") : task.trading_day}`,
        status: t(({queued:"排队中",running:"运行中",cancel_requested:"正在取消",succeeded:"已完成",failed:"失败",cancelled:"已取消",interrupted:"已中断"})[task.state]),
        completed: task.completed, total: task.total, open: () => context.navigate("workspace.research"),
    })),
    workspace: { id: "workspace.research", get title() { return t("研究"); }, icon: "⌘", get section() { return t("研究与回测"); }, component: context => <Panel {...context}/> },
};
