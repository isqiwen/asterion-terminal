import { lazy } from "react";
import type { TerminalFeature } from "../../src/host/feature-registry";
import { type MessageValues, translate } from "../../src/i18n";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
const LivePanel = lazy(() => import("./LivePanel").then(module => ({ default: module.LivePanel })));
const LiveStrategies = lazy(() =>
  import("./LiveStrategies").then(module => ({ default: module.LiveStrategies })),
);
export const feature: TerminalFeature = {
  id: "asterion.terminal.trading",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.trading",
      get title() {
        return t("工作台");
      },
      component: context => <LivePanel {...context} />,
    },
    {
      id: "workspace.trading-strategies",
      get title() {
        return t("策略");
      },
      component: context => <LiveStrategies {...context} />,
    },
  ],
};
