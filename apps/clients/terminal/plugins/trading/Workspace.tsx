import { translate, useWorkspaceDraft, type TerminalContext } from "../contract";
import { Panel } from "./Panel";
import { LivePanel } from "./LivePanel";
const t = (key: string) => translate("asterion.terminal.trading", key);

// Historical paper sessions and live CTP sessions are separate accounts.
export function Workspace(context: TerminalContext) {
  const [mode, setMode] = useWorkspaceDraft<"paper" | "live">("mode", () =>
    context.snapshot?.live && !context.snapshot.paper ? "live" : "paper",
  );
  return (
    <>
      <div className="trading-modes" role="group" aria-label={t("交易方式")}>
        <button aria-pressed={mode === "paper"} onClick={() => setMode("paper")}>
          {t("模拟")}
        </button>
        <button aria-pressed={mode === "live"} onClick={() => setMode("live")}>
          {t("实盘")}
        </button>
      </div>
      {mode === "live" ? <LivePanel {...context} /> : <Panel {...context} />}
    </>
  );
}
