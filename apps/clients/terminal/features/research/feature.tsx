import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import "./research.css";
import type { TerminalFeature } from "../contract";
const ResearchLab = lazy(() =>
  import("./ResearchLab").then(module => ({ default: module.ResearchLab })),
);
// The research page: strategies, factors and runs on the lab service.
export const feature: TerminalFeature = {
  id: "asterion.terminal.research",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.lab-research",
      get title() {
        return t("研究");
      },
      component: context => <ResearchLab {...context} />,
    },
  ],
};
