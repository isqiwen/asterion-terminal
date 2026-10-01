import "./data.css";
import { type TerminalContext, translate } from "../contract";
import { ArchivedDataPanel } from "./ArchivedDataPanel";
import { HistoricalDownloads } from "./HistoricalDownloads";

const t = (key: string) => translate("asterion.terminal.data-workbench", key);
export function DataPanel(context: TerminalContext) {
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
      </nav>
      {page === "history" ? (
        <HistoricalDownloads {...context} />
      ) : (
        <ArchivedDataPanel {...context} />
      )}
    </div>
  );
}
