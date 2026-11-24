import { useState } from "react";
import { FeedState } from "../../src/generated/asterion/market_pb";
import { useMarket } from "../../src/host/market";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { translate } from "../../src/i18n";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
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
export function MarketBar() {
  const market = useMarket();
  const [section, setSection] = useWorkspaceDraft("market-section", "期货全景");
  const [, setVenue] = useWorkspaceDraft("market-venue", "");
  const [scope, setScope] = useWorkspaceDraft("market-scope", "products");
  const [search, setSearch] = useWorkspaceDraft("quote-search", "");
  const [columns, setColumns] = useWorkspaceDraft("market-columns", defaultQuoteColumns);
  const [sort, setSort] = useWorkspaceDraft("market-sort", defaultSort);
  const [editingColumns, setEditingColumns] = useState(false);
  if (!market) return null;
  const feed = market.board.feed;
  const live = market.online && feed?.state === FeedState.CONNECTED;
  const phaseTone: Tone = live
    ? "ok"
    : !market.online || feed?.state === FeedState.FAILED
      ? "danger"
      : "attention";
  const phase = !market.online
    ? t("行情服务失联")
    : t(phases[feed?.state ?? FeedState.UNSPECIFIED] ?? "服务尚未连接柜台");
  return (
    <div className="market-bar">
      <nav className="market-sections tabs" aria-label={t("期货分类")}>
        {futuresSections.map(value => (
          <button
            key={value}
            type="button"
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
      <div className="market-scope tabs" role="group" aria-label={t("合约范围")}>
        <button
          type="button"
          aria-pressed={scope === "products"}
          disabled={section === "自选" || search.trim() !== ""}
          title={t("主连行是同品种持仓量最大的实际合约")}
          onClick={() => setScope("products")}
        >
          {t("品种概览")}
        </button>
        <button
          type="button"
          aria-pressed={scope === "months"}
          disabled={section === "主力合约" || section === "自选" || search.trim() !== ""}
          onClick={() => setScope("months")}
        >
          {t("全部月份")}
        </button>
      </div>
      <button
        type="button"
        className="market-bar-button"
        onClick={event => {
          event.currentTarget.focus();
          setEditingColumns(true);
        }}
      >
        {t("表头设置")}
      </button>
      <input
        type="search"
        className="market-search"
        aria-label={t("搜索市场合约")}
        placeholder={t("搜索市场合约")}
        spellCheck={false}
        autoCorrect="off"
        autoCapitalize="none"
        value={search}
        onChange={event => setSearch(event.target.value)}
      />
      <span className="market-bar-gap" />
      <StatusBadge tone={phaseTone}>
        <span role="status">{phase}</span>
      </StatusBadge>
      <MarketStatus />
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
