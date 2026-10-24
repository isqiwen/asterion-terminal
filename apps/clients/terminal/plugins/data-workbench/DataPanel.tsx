import "./data.css";
import { WorkloadAccess, type TerminalContext } from "../contract";
import { ArchivedDataPanel } from "./ArchivedDataPanel";
import { MaterialAcquisition } from "./MaterialAcquisition";
import { ObservationArchive } from "./ObservationArchive";
import { HistoricalDownloads } from "./HistoricalDownloads";
import { MarketTables } from "./MarketTables";

export function DataPanel(context: TerminalContext) {
  return (
    <WorkloadAccess role="data" context={context} enabled={!context.snapshot?.data}>
      {ready => <DataContent {...ready} />}
    </WorkloadAccess>
  );
}
function DataContent(context: TerminalContext) {
  const page = context.workspacePage ?? "history";
  return (
    <div className="data-workbench">
      {page === "history" ? (
        <HistoricalDownloads {...context} />
      ) : page === "materials" ? (
        <MaterialAcquisition context={context} />
      ) : page === "tables" ? (
        <MarketTables {...context} />
      ) : (
        <>
          <ArchivedDataPanel {...context} />
          <ObservationArchive context={context} />
        </>
      )}
    </div>
  );
}
