import { HeaderSearch } from "./HeaderSearch";
import { Icon } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
const Panel = lazy(() => import("./Workspace").then(module => ({ default: module.Workspace })));
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.futures-market",
  commands: [
    "market.local",
    "market.connect",
    "market.credentials.save",
    "market.credentials.clear",
    "market.catalog",
    "market.subscribe",
    "market.disconnect",
    "market.minutes",
    "data.minutes.page",
    "data.datasets",
    "data.tables.coverage",
    "data.tables.daily",
    "data.tables.products",
    "data.tables.listed_contracts",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  workspace: {
    id: "workspace.market",
    hideToolbar: true,
    get title() {
      return t("市场");
    },
    icon: <Icon name="market" />,
    component: context => <Panel {...context} />,
    headerTools: HeaderSearch,
  },
};

const Watchlist = lazy(() =>
  import("./QuoteWorkspaces").then(module => ({ default: module.WatchlistWorkspace })),
);
const Contract = lazy(() =>
  import("./QuoteWorkspaces").then(module => ({ default: module.ContractWorkspace })),
);
export const watchlistPlugin: TerminalPlugin = {
  id: "asterion.terminal.watchlist",
  commands: plugin.commands,
  workspace: {
    id: "workspace.watchlist",
    hideToolbar: true,
    get title() {
      return t("自选");
    },
    icon: <Icon name="star" />,
    component: context => <Watchlist {...context} />,
    headerTools: context => <HeaderSearch {...context} target="workspace.watchlist" />,
  },
};
export const contractPlugin: TerminalPlugin = {
  id: "asterion.terminal.contract",
  commands: plugin.commands,
  workspace: {
    id: "workspace.contract",
    hideToolbar: true,
    get title() {
      return t("合约");
    },
    icon: <Icon name="contract" />,
    component: context => <Contract {...context} />,
    headerTools: context => <HeaderSearch {...context} target="workspace.contract" />,
  },
};
