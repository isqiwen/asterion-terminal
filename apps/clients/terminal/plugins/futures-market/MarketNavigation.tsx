import { translate, useWorkspaceDraft, type TerminalContext } from "../contract";
import { MarketModes } from "./Navigation";
import { useEffect, useRef } from "react";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
// Only categories with a connected source: CTP futures, overall, dominant and by exchange.
export const futuresSections = [
  "期货全景",
  "主力合约",
  "中金所",
  "上期所",
  "大商所",
  "郑商所",
  "广期所",
  "上能源",
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
      // A modal opened from this menu owns outside clicks and Escape. Keep
      // its trigger mounted so closing the modal can restore keyboard focus.
      if (document.querySelector("dialog[open]")) return;
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
  const [section, setSection] = useWorkspaceDraft("market-section", "期货全景");
  const [, setVenue] = useWorkspaceDraft("market-venue", "");
  const [, setScope] = useWorkspaceDraft("market-scope", "products");
  const [, setSearch] = useWorkspaceDraft("quote-search", "");
  return (
    <div className="market-navigation">
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
      <MarketModes {...context} />
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
