import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import "./research.css";
import type { TerminalFeature } from "../contract";
import { StrategyLab } from "./StrategyLab";
import { FactorLab } from "./FactorLab";
// The research views: strategies and their runs, factors and their evaluations, on the lab service.
export const feature: TerminalFeature = {
  id: "asterion.terminal.research",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.lab-strategies",
      get title() {
        return t("策略");
      },
      component: context => <StrategyLab {...context} />,
    },
    {
      id: "workspace.lab-factors",
      get title() {
        return t("因子");
      },
      component: context => <FactorLab {...context} />,
    },
  ],
};
