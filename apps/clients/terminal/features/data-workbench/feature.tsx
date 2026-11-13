import type { TerminalContext, TerminalFeature } from "../../src/host/feature-registry";
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import "./data.css";
import { Browse } from "./Browse";
import { Coverage } from "./Coverage";
import { Overview } from "./Overview";
import { t } from "./dataset";
// The data views read the remote Warehouse dataset and its acquisitions.
export const feature: TerminalFeature = {
  id: "asterion.terminal.data-workbench",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.data-overview",
      get title() {
        return t("概览");
      },
      component: (context: TerminalContext) => <Overview {...context} />,
    },
    {
      id: "workspace.data-coverage",
      get title() {
        return t("覆盖");
      },
      component: (context: TerminalContext) => <Coverage {...context} />,
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
