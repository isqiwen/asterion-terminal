import type { TerminalContext } from "../contract";
import { LivePanel } from "./LivePanel";
import { MarketNavigation } from "./MarketNavigation";
export function Workspace(context: TerminalContext) {
  return (
    <div className="market-workspace">
      <MarketNavigation />
      <LivePanel context={context} />
    </div>
  );
}
