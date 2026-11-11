import type { TerminalFeature } from "../../src/host/feature-registry";
import { type MessageValues, translate } from "../../src/i18n";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { Workspace as Panel } from "./Workspace";

export const feature: TerminalFeature = {
  id: "asterion.terminal.futures-market",
  languageResources: { "zh-CN": zh, "en-US": en },
  workspaces: [
    {
      id: "workspace.market",
      hideToolbar: true,
      get title() {
        return t("市场");
      },

      component: context => <Panel {...context} />,
    },
  ],
};
