import type { TerminalFeature } from "../../src/host/feature-registry";
import { type MessageValues, translate } from "../../src/i18n";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import "./research.css";
import { StrategyLab } from "./StrategyLab";
import { FactorLab } from "./FactorLab";
import { Runs } from "./Runs";
import { Evaluations } from "./Evaluations";
// The research views: strategies, their runs and reports, factors and their evaluations, on the lab service.
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
      id: "workspace.lab-runs",
      get title() {
        return t("回测");
      },
      component: context => <Runs {...context} />,
    },
    {
      id: "workspace.lab-factors",
      get title() {
        return t("因子");
      },
      component: context => <FactorLab {...context} />,
    },
    {
      id: "workspace.lab-evaluations",
      get title() {
        return t("因子评价");
      },
      component: context => <Evaluations {...context} />,
    },
  ],
};
