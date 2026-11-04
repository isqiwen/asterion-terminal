import { HeaderSearch } from "./HeaderSearch";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import type { TerminalFeature } from "../contract";
import { Workspace as Panel } from "./Workspace";
import { ContractWorkspace as Contract } from "./QuoteWorkspaces";

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
      headerTools: HeaderSearch,
    },
    {
      id: "workspace.contract",
      hideToolbar: true,
      get title() {
        return t("合约");
      },

      component: context => <Contract {...context} />,
      headerTools: context => <HeaderSearch {...context} target="workspace.contract" />,
    },
  ],
};
