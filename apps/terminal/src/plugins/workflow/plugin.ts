import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "./context";
import { Settings } from "./Settings";
import type { SettingsContext } from "./settingsContext";
import { Workspace } from "./Workspace";
export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.workflow",
  requires: [
    "asterion.identity",
    "asterion.data",
    "asterion.market",
    "asterion.research",
    "asterion.tasks",
    "asterion.appearance",
    "asterion.maintenance",
  ],
  screens: [
    { id: "terminal.workspace", component: Workspace },
    { id: "terminal.settings", component: Settings },
  ],
};
