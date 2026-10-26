import type { TerminalContext } from "../contract";
import { MarketPanel } from "./MarketPanel";
import { LivePanel } from "./LivePanel";
import { MarketNavigation } from "./MarketNavigation";
export function Workspace(context: TerminalContext) {
  return (
    <div className="market-workspace">
      <MarketNavigation context={context} />
      {context.marketMode === "history" ? (
        <MarketPanel context={context} />
      ) : (
        <LivePanel context={context} />
      )}
    </div>
  );
}
