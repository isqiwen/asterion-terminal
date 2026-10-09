import { lazy } from "react";
import { DataTaskAccess, useWorkspaceDraft, type TerminalContext } from "../contract";
import "./research.css";
import { t } from "./shared";
const Backtest = lazy(() => import("./Backtest").then(module => ({ default: module.Backtest })));
const Factor = lazy(() => import("./Factor").then(module => ({ default: module.Factor })));
const pages = { backtest: "回测", factor: "因子" };
type Page = keyof typeof pages;

export function Workspace(context: TerminalContext) {
  const [last, setLast] = useWorkspaceDraft<Page>("research-page", "backtest");
  // A task opens on the page that started it.
  const requested =
    context.workspacePage === "task"
      ? context.workspaceParams?.kind === "backtest"
        ? "backtest"
        : "factor"
      : context.workspacePage;
  const page = requested && requested in pages ? (requested as Page) : last;
  const Current = page === "backtest" ? Backtest : Factor;
  return (
    <>
      <nav className="research-modes" aria-label={t("研究页面")}>
        {Object.entries(pages).map(([id, label]) => (
          <button
            key={id}
            aria-pressed={page === id}
            onClick={() => {
              setLast(id as Page);
              context.navigate("workspace.research", { page: id });
            }}
          >
            {t(label)}
          </button>
        ))}
      </nav>
      <DataTaskAccess context={context}>
        {ready => (
          <Current {...ready} workspacePage={context.workspacePage === "task" ? "task" : page} />
        )}
      </DataTaskAccess>
    </>
  );
}
