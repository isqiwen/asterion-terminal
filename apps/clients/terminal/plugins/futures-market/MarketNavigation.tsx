import { translate, useWorkspaceDraft, type TerminalContext } from "../contract";
import { MarketModes } from "./Navigation";
import { useEffect, useRef } from "react";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
export const marketAssets = ["期货", "期权", "外汇", "黄金", "外盘", "股票", "环球"];
export const futuresSections = [
  "期货全景",
  "主力合约",
  "商品指数",
  "波动率指数",
  "行业分类",
  "中金所",
  "上期所",
  "大商所",
  "大商所月均价",
  "郑商所",
  "广期所",
  "上能源",
  "期货夜盘",
];
export const sectionVenues: Record<string, string> = {
  中金所: "CFFEX",
  上期所: "SHFE",
  大商所: "DCE",
  郑商所: "CZCE",
  广期所: "GFEX",
  上能源: "INE",
};
export function MarketNavigation({ context }: { context: TerminalContext }) {
  const [menuOpen, setMenuOpen] = useWorkspaceDraft("market-menu", false);
  const menu = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!menuOpen) return;
    const close = (event: PointerEvent | KeyboardEvent) => {
      if (
        event instanceof KeyboardEvent
          ? event.key === "Escape"
          : !menu.current?.contains(event.target as Node)
      )
        setMenuOpen(false);
    };
    window.addEventListener("pointerdown", close);
    window.addEventListener("keydown", close);
    return () => {
      window.removeEventListener("pointerdown", close);
      window.removeEventListener("keydown", close);
    };
  }, [menuOpen, setMenuOpen]);
  const [asset, setAsset] = useWorkspaceDraft("market-asset", "期货");
  const [section, setSection] = useWorkspaceDraft("market-section", "期货全景");
  const [, setVenue] = useWorkspaceDraft("market-venue", "");
  const [, setScope] = useWorkspaceDraft("market-scope", "products");
  const [, setSearch] = useWorkspaceDraft("quote-search", "");
  return (
    <div className="market-navigation">
      <nav className="market-assets" aria-label={t("资产类别")}>
        {marketAssets.map(value => (
          <button key={value} aria-pressed={asset === value} onClick={() => setAsset(value)}>
            {t(value)}
          </button>
        ))}
        <MarketModes {...context} />
      </nav>
      {asset === "期货" && (
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
      )}
      <div className="market-menu-anchor" ref={menu}>
        <button
          className="market-menu-button"
          aria-label={t("行情设置")}
          title={t("行情设置")}
          aria-expanded={menuOpen}
          onClick={() => setMenuOpen(!menuOpen)}
        >
          ⋯
        </button>
        {menuOpen && (
          <div className="market-menu" role="dialog" aria-label={t("行情设置")}>
            <div id="market-menu-slot" />
          </div>
        )}
      </div>
    </div>
  );
}
export function useMarketSection() {
  const [asset] = useWorkspaceDraft("market-asset", "期货");
  const [section] = useWorkspaceDraft("market-section", "期货全景");
  const available =
    asset === "期货" &&
    (section === "期货全景" || section === "主力合约" || !!sectionVenues[section]);
  return { asset, section, available };
}
