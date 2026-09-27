import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) => translate("asterion.terminal.data-workbench", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
const Panel = lazy(() => import("./DataPanel").then(module => ({ default: module.DataPanel })));
export const plugin: TerminalPlugin = {
    id: "asterion.terminal.data-workbench", apiVersion: 1, languageResources: { "zh-CN": zh, "en-US": en },
    tasks: context => (context.snapshot?.research?.tasks??[]).filter(task=>(task.kind==="data_import"||task.kind==="calendar_import")).map(task=>({
        id:task.id, title:`${task.source_name} · ${task.instrument}`, status:t(({queued:"排队中",running:"发布中",cancel_requested:"正在取消",succeeded:"已发布",failed:"失败",cancelled:"已取消",interrupted:"已中断"})[task.state]), completed:task.completed,total:task.total,open:()=>context.navigate("workspace.data"),
    })),
    workspace: { id: "workspace.data", get title() { return t("数据"); }, icon: "▤", get section() { return t("数据管理"); }, component: context => <Panel {...context} busy={context.busy||!context.snapshot}/> },
    widgets: context => {
        const data = context.snapshot?.dataset;
        const openData = () => context.navigate("workspace.data");
        return [
            { id: "data.local", title: t("数据工作台"), category: t("数据"), description: t("本地数据校验与预览"), width: 2, column: "primary", defaultVisible: true, hasContent: !!data,
                render: () => <div className="overview-data"><strong>{data ? data.filename : t("尚未导入历史数据")}</strong><p>{data ? t(data.persistent?"已加载发布版本":"已完成逐笔校验 · 仅当前会话预览") : t("校验实际合约、交割月份、价格步长和成交手数。")}</p><button onClick={openData}>{t("打开数据工作区")}</button></div> },
        ];
    },
};
