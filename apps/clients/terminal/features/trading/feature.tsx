import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import type { TerminalFeature } from "../contract";
import { LivePanel } from "./LivePanel";
export const feature: TerminalFeature = {
  id: "asterion.terminal.trading",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.trading",
      get title() {
        return t("交易");
      },

      component: context => <LivePanel {...context} />,
    },
  ],
};
