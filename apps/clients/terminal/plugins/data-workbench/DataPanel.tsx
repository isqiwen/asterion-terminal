import "./data.css";
import { DataTaskAccess, type TerminalContext } from "../contract";
import { ArchivedDataPanel } from "./ArchivedDataPanel";
import { HistoricalDownloads } from "./HistoricalDownloads";
import { MarketTables } from "./MarketTables";

export function DataPanel(context: TerminalContext) {
  return (
    <DataTaskAccess context={context} enabled={!context.snapshot?.data}>
      {ready => <DataContent {...ready} />}
    </DataTaskAccess>
  );
}
function DataContent(context: TerminalContext) {
  const page = context.workspacePage ?? "history";
  return (
    <div className="data-workbench">
      {page === "history" ? (
        <HistoricalDownloads {...context} />
      ) : page === "tables" ? (
        <MarketTables {...context} />
      ) : (
        <ArchivedDataPanel {...context} />
      )}
    </div>
  );
}
