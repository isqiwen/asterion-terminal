import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) => translate("asterion.terminal.overview", key, values);
import { ServiceSummary } from "./ServiceSummary";
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
const Panel = lazy(() => import("./Dashboard").then(module => ({ default: module.Dashboard })));
export const plugin: TerminalPlugin = {
    id: "asterion.terminal.overview", apiVersion: 1, languageResources: { "zh-CN": zh, "en-US": en },
    workspace: { id: "workspace.overview", get title() { return t("总览"); }, icon: "▦", get section() { return t("工作概览"); }, component: context => <Panel status={<ServiceSummary context={context}/>} widgets={context.widgets} storageKey="asterion.cpp-terminal.dashboard.v1" refresh={context.refresh} catalogError={context.error} openTasks={context.openTasks}/> },
};
