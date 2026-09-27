import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) => translate("asterion.terminal.futures-market", key, values);
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
import { lazy } from "react";
import type { TerminalPlugin } from "../contract";
import { QuoteTable, marketPhase } from "./QuoteTable";
const Panel = lazy(() => import("./Workspace").then(module => ({ default: module.Workspace })));
export const plugin: TerminalPlugin = {
    id: "asterion.terminal.futures-market", apiVersion: 1, languageResources: { "zh-CN": zh, "en-US": en },
    workspace: { id: "workspace.market", get title() { return t("市场"); }, icon: "⌁", get section() { return t("行情全景"); }, component: context => <Panel {...context}/> },
    widgets: context => {
        const market = context.snapshot?.market;
        const openMarket = () => context.navigate("workspace.market", {marketMode: "live"});
        return [
            { id: "market.quotes", title: t("自选行情"), category: t("市场"), description: t("实时报价与行情时间"), width: 2, column: "primary", defaultVisible: true,
                render: () => <><div className="overview-card-bar"><span>{marketPhase(market)}</span>{!!market?.subscriptions.length && <button onClick={openMarket}>{t("管理自选")}</button>}</div>{market?.subscriptions.length ? <QuoteTable market={market} compact/> : <div className="overview-market-empty"><span className="overview-market-icon" aria-hidden="true">⌁</span><strong>{t("关注你的期货合约")}</strong><p>{t("连接行情，查看自选合约的实时报价。")}</p><button className="primary" onClick={openMarket}>{t("连接行情")}</button></div>}</> },
        ];
    },
};
