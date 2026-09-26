import type { UiModule } from "@asterion/workbench/extensions/modules";
import type { PanelContext } from "./context";
import type { SettingsContext } from "./context";
import { Dashboard } from "./Dashboard";

export const uiModule = { 
  id: "asterion.ui.overview",
  workspaces: [{
    id: "workspace.overview", title: "总览", icon: "▦",
    sections: [{ title: "工作概览", panel: "overview.summary" }],
  }],
  panels: [{ id: "overview.summary", scope: "overview",
    render: (context) => <Dashboard {...context} /> }],
} satisfies UiModule<PanelContext, SettingsContext>;
