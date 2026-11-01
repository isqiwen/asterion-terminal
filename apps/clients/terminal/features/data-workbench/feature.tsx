import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import "./data.css";
import type { TerminalContext, TerminalFeature } from "../contract";
const Coverage = lazy(() => import("./Coverage").then(module => ({ default: module.Coverage })));
// The data page reads the remote Market warehouse, acquisitions and recording status.
export const feature: TerminalFeature = {
  id: "asterion.terminal.data-workbench",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.coverage",
      get title() {
        return t("数据覆盖");
      },
      component: (context: TerminalContext) => <Coverage {...context} />,
    },
  ],
};
