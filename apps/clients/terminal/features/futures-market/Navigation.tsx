import { translate, type TerminalContext } from "../contract";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
export function MarketModes(context: TerminalContext) {
  const mode = context.marketMode ?? "live";
  const setMode = (marketMode: "live" | "history") =>
    context.navigate("workspace.market", { marketMode });
  return (
    <div className="market-modes" role="tablist" aria-label={t("行情来源")}>
      <button role="tab" aria-selected={mode === "live"} onClick={() => setMode("live")}>
        {t("实时行情")}
      </button>
      <button role="tab" aria-selected={mode === "history"} onClick={() => setMode("history")}>
        {t("历史行情")}
      </button>
    </div>
  );
}
