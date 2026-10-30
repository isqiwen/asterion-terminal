import { HeaderSearch } from "./HeaderSearch";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalFeature } from "../contract";
const Panel = lazy(() => import("./Workspace").then(module => ({ default: module.Workspace })));
const Watchlist = lazy(() =>
  import("./QuoteWorkspaces").then(module => ({ default: module.WatchlistWorkspace })),
);
const Contract = lazy(() =>
  import("./QuoteWorkspaces").then(module => ({ default: module.ContractWorkspace })),
);

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
      id: "workspace.watchlist",
      hideToolbar: true,
      get title() {
        return t("自选");
      },

      component: context => <Watchlist {...context} />,
      headerTools: context => <HeaderSearch {...context} target="workspace.watchlist" />,
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
