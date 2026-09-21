import { Research } from "./Research";

import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
function ResearchRuns(context: PanelContext["research"], active: boolean) {
  const { connection, accountEmail, externalRefill, inspectData, submitted } =
    context;
  return (
    <Research
      key={accountEmail}
      accountEmail={accountEmail}
      api={connection.api}
      connected={connection.connected && active}
      externalRefill={externalRefill}
      onInspectData={inspectData}
      onSubmitted={submitted}
    />
  );
}

export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.research",
  extensions: [
    {
      id: "research.backtest",
      point: "tasks.views",
      value: {
        id: "research.backtest",
        title: "日线回测",
        result: () => "研究结果已发布 · 在研究页查看",
      },
    },
  ],
  requires: ["asterion.data", "asterion.contract_rules"],
  workspaces: [
    {
      id: "workspace.research",
      title: "研究",
      icon: "⌘",
      sections: [{ title: "实验与运行", panel: "research.runs" }],
    },
  ],
  panels: [
    {
      id: "research.runs",
      scope: "research",
      keepMounted: true,
      render: ResearchRuns,
    },
  ],
};
