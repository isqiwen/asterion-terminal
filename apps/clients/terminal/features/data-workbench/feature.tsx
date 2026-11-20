import { lazy } from "react";
import type { TerminalContext, TerminalFeature } from "../../src/host/feature-registry";
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import "./data.css";
const Browse = lazy(() => import("./Browse").then(module => ({ default: module.Browse })));
const Dataset = lazy(() => import("./DatasetView").then(module => ({ default: module.Dataset })));
import { t } from "./dataset";
// The data views read the remote Warehouse dataset and its acquisitions.
export const feature: TerminalFeature = {
  id: "asterion.terminal.data-workbench",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.data-dataset",
      get title() {
        return t("数据覆盖");
      },
      component: (context: TerminalContext) => <Dataset {...context} />,
    },
    {
      id: "workspace.data-browse",
      get title() {
        return t("浏览");
      },
      component: (context: TerminalContext) => <Browse {...context} />,
    },
  ],
};
