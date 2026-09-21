import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
import { Dashboard } from "./Dashboard";

export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.overview",
  requires: [],
  workspaces: [{
    id: "workspace.overview", title: "总览", icon: "▦",
    sections: [{ title: "工作概览", panel: "overview.summary" }],
  }],
  panels: [{ id: "overview.summary", scope: "overview",
    render: (context) => <Dashboard {...context} /> }],
};
