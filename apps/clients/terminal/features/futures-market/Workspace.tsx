import { useMarket } from "../../src/host/market";
import { translate, type TerminalContext } from "../contract";
import { MarketBar } from "./MarketBar";
import { MarketBoard } from "./MarketBoard";
import { MarketUnavailable } from "./MarketUnavailable";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

export function Workspace(context: TerminalContext) {
  const market = useMarket();
  return (
    <div className="market-workspace">
      <MarketBar context={context} />
      <section className="live-market" aria-label={t("实时期货行情")}>
        {market ? (
          <MarketBoard market={market} context={context} />
        ) : (
          <MarketUnavailable context={context} />
        )}
      </section>
    </div>
  );
}
