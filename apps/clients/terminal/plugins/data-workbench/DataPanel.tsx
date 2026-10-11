import "./data.css";
import { DataTaskAccess, type TerminalContext, translate } from "../contract";
import { ArchivedDataPanel } from "./ArchivedDataPanel";
import { HistoricalDownloads } from "./HistoricalDownloads";
import { MarketTables } from "./MarketTables";

const t = (key: string) => translate("asterion.terminal.data-workbench", key);
export function DataPanel(context: TerminalContext) {
  return (
    <DataTaskAccess context={context} enabled={!context.snapshot?.data}>
      {ready => <DataContent {...ready} />}
    </DataTaskAccess>
  );
}
function DataContent(context: TerminalContext) {
  const page = context.workspacePage ?? "history";
  const setPage = (page: string) => context.navigate("workspace.data", { page });
  return (
    <div className="data-workbench">
      <nav className="data-navigation" aria-label={t("数据页面")}>
        <button
          aria-current={page === "history" ? "page" : undefined}
          onClick={() => setPage("history")}
        >
          {t("历史数据")}
        </button>
        <button
          aria-current={page === "records" ? "page" : undefined}
          onClick={() => setPage("records")}
        >
          {t("历史数据仓库")}
        </button>
        <button
          aria-current={page === "tables" ? "page" : undefined}
          onClick={() => setPage("tables")}
        >
          {t("全市场数据")}
        </button>
      </nav>
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
