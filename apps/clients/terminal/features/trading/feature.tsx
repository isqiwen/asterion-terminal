import type { TerminalFeature } from "../../src/host/feature-registry";
import { type MessageValues, translate } from "../../src/i18n";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { LivePanel } from "./LivePanel";
import { LiveStrategies } from "./LiveStrategies";
export const feature: TerminalFeature = {
  id: "asterion.terminal.trading",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.trading",
      get title() {
        return t("账户");
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
