import { lazy } from "react";
import { DataTaskAccess, translate, useWorkspaceDraft, type TerminalContext } from "../contract";
import "./backtest-factor.css";
const Panel = lazy(() => import("./Panel").then(module => ({ default: module.Panel })));
const pages = {
  backtest: "均线回测",
  factor: "因子分析",
  daily_factor: "日线因子",
};
const t = (key: string) => translate("asterion.terminal.backtest-factor", key);

export function Workspace(context: TerminalContext) {
  const [last, setLast] = useWorkspaceDraft<keyof typeof pages>("backtest-factor-page", "backtest");
  const requested =
    context.workspacePage === "task" ? context.workspaceParams?.kind : context.workspacePage;
  const page = requested && requested in pages ? (requested as keyof typeof pages) : last;
  return (
    <>
      <nav className="backtest-factor-modes" aria-label={t("回测与因子方式")}>
        {Object.entries(pages).map(([id, label]) => (
          <button
            key={id}
            aria-pressed={page === id}
            onClick={() => {
              setLast(id as keyof typeof pages);
              context.navigate("workspace.backtest-factor", { page: id });
            }}
          >
            {t(label)}
          </button>
        ))}
      </nav>
      <DataTaskAccess context={context}>
        {ready => (
          <Panel
            {...ready}
            mode={page}
            workspacePage={context.workspacePage === "task" ? "task" : page}
          />
        )}
      </DataTaskAccess>
    </>
  );
}
