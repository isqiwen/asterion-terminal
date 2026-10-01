import { HeaderSearch } from "./HeaderSearch";
import { Icon } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
import { QuoteTable, marketPhase } from "./QuoteTable";
const Panel = lazy(() => import("./Workspace").then(module => ({ default: module.Workspace })));
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.futures-market",
  apiVersion: 1,
  commands: [
    "market.local",
    "market.connect",
    "market.catalog",
    "market.subscribe",
    "market.disconnect",
    "market.minutes",
    "research.minutes.page",
    "research.daily.page",
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
  widgets: context => {
    const source = context.snapshot?.market;
    const market = source
      ? {
          ...source,
          subscriptions: source.subscriptions.filter(row =>
            source.watchlist?.some(id => id.venue === row.venue && id.symbol === row.symbol),
          ),
        }
      : source;
    const openMarket = () => context.navigate("workspace.market", { marketMode: "live" });
    return [
      {
        id: "market.quotes",
        title: t("自选行情"),
        category: t("市场"),
        description: t("实时报价与行情时间"),
        width: 2,
        column: "primary",
        defaultVisible: true,
        render: () => (
          <>
            <div className="overview-card-bar">
              <span>{marketPhase(market)}</span>
              {!!market?.subscriptions.length && (
                <button onClick={openMarket}>{t("管理自选")}</button>
              )}
            </div>
            {market?.subscriptions.length ? (
              <QuoteTable market={market} compact />
            ) : (
              <div className="overview-market-empty">
                <span className="overview-market-icon" aria-hidden="true">
                  <Icon name="market" size={28} />
                </span>
                <strong>{t("关注你的期货合约")}</strong>
                <p>{t("连接行情，查看自选合约的实时报价。")}</p>
                <button className="primary" onClick={openMarket}>
                  {t("连接行情")}
                </button>
              </div>
            )}
          </>
        ),
      },
    ];
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
  apiVersion: 1,
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
  apiVersion: 1,
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
