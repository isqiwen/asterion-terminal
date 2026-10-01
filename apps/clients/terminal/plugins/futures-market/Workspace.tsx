import { translate, type TerminalContext } from "../contract";
import { MarketPanel } from "./MarketPanel";
import { LivePanel } from "./LivePanel";
import { MarketNavigation, useMarketSection } from "./MarketNavigation";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
export function Workspace(context: TerminalContext) {
  const { asset, section, available } = useMarketSection();
  return (
    <div className="market-workspace">
      <MarketNavigation context={context} />
      {!available ? (
        <section className="market-source-unavailable" aria-label={t("行情接入状态")}>
          <h2>{t(asset === "期货" ? section : asset)}</h2>
          <p role="status">
            {t(
              asset !== "期货"
                ? "该资产行情源尚未接入"
                : section === "行业分类"
                  ? "行业分类目录尚未接入"
                  : section === "期货夜盘"
                    ? "夜盘交易时段目录尚未接入"
                    : section === "大商所月均价"
                      ? "大商所月均价数据尚未接入"
                      : "该指数行情源尚未接入",
            )}
          </p>
        </section>
      ) : context.marketMode === "history" ? (
        <MarketPanel context={context} />
      ) : (
        <LivePanel context={context} />
      )}
    </div>
  );
}
