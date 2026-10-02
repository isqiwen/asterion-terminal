import { lazy } from "react";
import { ResearchAccess, translate, useWorkspaceDraft, type TerminalContext } from "../contract";
import "./research.css";
const Panel = lazy(() => import("./Panel").then(module => ({ default: module.Panel })));
const pages = {
  backtest: "均线回测",
  factor: "因子分析",
  daily_factor: "日线因子",
};
const t = (key: string) => translate("asterion.terminal.research", key);

export function Workspace(context: TerminalContext) {
  const [last, setLast] = useWorkspaceDraft<keyof typeof pages>("research-page", "backtest");
  const requested =
    context.workspacePage === "task" ? context.workspaceParams?.kind : context.workspacePage;
  const page = requested && requested in pages ? (requested as keyof typeof pages) : last;
  return (
    <>
      <nav className="research-modes" aria-label={t("研究方式")}>
        {Object.entries(pages).map(([id, label]) => (
          <button
            key={id}
            aria-pressed={page === id}
            onClick={() => {
              setLast(id as keyof typeof pages);
              context.navigate("workspace.research", { page: id });
            }}
          >
            {t(label)}
          </button>
        ))}
      </nav>
      <ResearchAccess context={context}>
        {ready => (
          <Panel {...ready} workspacePage={context.workspacePage === "task" ? "task" : page} />
        )}
      </ResearchAccess>
    </>
  );
}
