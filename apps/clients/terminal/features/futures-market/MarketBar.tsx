import { useState } from "react";
import { CatalogState, FeedState } from "../../src/generated/asterion/market_pb";
import { useMarket } from "../../src/host/market";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { translate } from "../../src/i18n";
import { BackendError, ErrorNotice } from "../../src/i18n/errors";
import { defaultSort } from "./MarketBoard";
import { MarketStatus } from "./MarketStatus";
import { QuoteColumns, defaultQuoteColumns } from "./QuoteColumns";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

// The watchlist, then categories with a connected source: overall, dominant and by exchange.
export const futuresSections = [
  "自选",
  "期货全景",
  "主力合约",
  "中金所",
  "上期所",
  "大商所",
  "郑商所",
  "广期所",
  "能源中心",
];
export const sectionVenues: Record<string, string> = {
  中金所: "CFFEX",
  上期所: "SHFE",
  大商所: "DCE",
  郑商所: "CZCE",
  广期所: "GFEX",
  能源中心: "INE",
};
const phases: Partial<Record<FeedState, string>> = {
  [FeedState.NO_SDK]: "服务尚未连接柜台",
  [FeedState.DISCONNECTED]: "服务尚未连接柜台",
  [FeedState.CONNECTING]: "连接中",
  [FeedState.LOGGING_IN]: "登录中",
  [FeedState.CONNECTED]: "已连接",
  [FeedState.RECONNECTING]: "重连中",
  [FeedState.FAILED]: "连接失败",
};

// One row above the board: what is listed, how, and whether the feed is live.
// Anything wrong with the feed shows in full under it.
export function MarketBar({ context }: { context: TerminalContext }) {
  const market = useMarket();
  const [section, setSection] = useWorkspaceDraft("market-section", "期货全景");
  const [, setVenue] = useWorkspaceDraft("market-venue", "");
  const [scope, setScope] = useWorkspaceDraft("market-scope", "products");
  const [search, setSearch] = useWorkspaceDraft("quote-search", "");
  const [columns, setColumns] = useWorkspaceDraft("market-columns", defaultQuoteColumns);
  const [sort, setSort] = useWorkspaceDraft("market-sort", defaultSort);
  const [editingColumns, setEditingColumns] = useState(false);
  const { error, attempt } = useActionError();
  if (!market) return null;
  const saved = context.snapshot?.market_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const feed = market.board.feed;
  const live = market.online && feed?.state === FeedState.CONNECTED;
  const phase = !market.online
    ? t("行情服务失联")
    : t(phases[feed?.state ?? FeedState.UNSPECIFIED] ?? FeedState[feed?.state ?? 0]);
  return (
    <div className="market-bar">
      <nav className="market-sections" aria-label={t("期货分类")}>
        {futuresSections.map(value => (
          <button
            key={value}
            aria-pressed={section === value}
            onClick={() => {
              setSection(value);
              setVenue(sectionVenues[value] ?? "");
              setScope(sectionVenues[value] ? "months" : "products");
              setSearch("");
            }}
          >
            {t(value)}
          </button>
        ))}
      </nav>
      <div className="market-scope" role="group" aria-label={t("合约范围")}>
        <button
          aria-pressed={scope === "products"}
          title={t("主连行显示同品种持仓量最大的实际月份合约，不是拼接的连续合约。")}
          onClick={() => setScope("products")}
        >
          {t("品种概览")}
        </button>
        <button aria-pressed={scope === "months"} onClick={() => setScope("months")}>
          {t("全部月份")}
        </button>
      </div>
      <button className="market-bar-button" onClick={() => setEditingColumns(true)}>
        {t("表头设置")}
      </button>
      <input
        type="search"
        className="market-search"
        aria-label={t("搜索市场合约")}
        placeholder={t("搜索市场合约")}
        value={search}
        onChange={event => setSearch(event.target.value)}
      />
      <span className="market-bar-gap" />
      <span className="market-phase" role="status" data-live={live || undefined}>
        {phase}
      </span>
      <MarketStatus />
      <button
        className="market-bar-button"
        disabled={context.busy}
        title={endpoint ? `${endpoint.host}:${endpoint.port}` : undefined}
        onClick={() => void attempt(() => context.trade("market.close"))}
      >
        {t("断开")}
      </button>
      <div className="market-bar-alerts">
        <ActionError error={error} namespace="asterion.terminal.futures-market" />
        {!!market.error && (
          <p role="alert">
            {t("行情连接失败")}: {market.error}
          </p>
        )}
        {!!feed?.ctpError && (
          <p role="alert">
            {t("行情连接异常，请检查配置或服务时段。")} CTP {feed.ctpError}
          </p>
        )}
        {market.board.catalog?.status?.state === CatalogState.FAILED && (
          <ErrorNotice
            error={new BackendError("operation_failed", market.board.catalog.status.error)}
          />
        )}
      </div>
      {editingColumns && (
        <QuoteColumns
          value={columns}
          close={() => setEditingColumns(false)}
          apply={next => {
            setColumns(next);
            if (
              sort.column !== "合约" &&
              !next.some(item => item.column === sort.column && item.visible)
            )
              setSort({ column: "合约", direction: "ascending" });
          }}
        />
      )}
    </div>
  );
}
