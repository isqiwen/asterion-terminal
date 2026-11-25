import { lazy } from "react";
import type { TerminalFeature } from "../../src/host/feature-registry";
import { type MessageValues, translate } from "../../src/i18n";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import "./research.css";
const StrategyLab = lazy(() =>
  import("./StrategyLab").then(module => ({ default: module.StrategyLab })),
);
const FactorLab = lazy(() => import("./FactorLab").then(module => ({ default: module.FactorLab })));
const Runs = lazy(() => import("./Runs").then(module => ({ default: module.Runs })));
// The research views: strategies, their runs and reports, and factors with their evaluations, on the lab service.
export const feature: TerminalFeature = {
  id: "asterion.terminal.research",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.lab-factors",
      get title() {
        return t("因子");
      },
      component: context => <FactorLab {...context} />,
    },
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
  ],
};
