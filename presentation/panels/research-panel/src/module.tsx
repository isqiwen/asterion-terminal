import { Research } from "./Research";

import type { UiModule } from "@asterion/workbench/extensions/modules";
import type { PanelContext } from "./context";
import type { SettingsContext } from "./context";
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

export const uiModule = { 
  id: "asterion.ui.research-panel",
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
} satisfies UiModule<PanelContext, SettingsContext>;
