import type { UiModule } from "@asterion/workbench/extensions/modules";
import type { PanelContext } from "./context";
import { Settings } from "./Settings";
import type { SettingsContext } from "./settingsContext";
import { Workspace } from "./Workspace";
export const uiModule = { 
  id: "asterion.ui.terminal-workspace",
  screens: [
    { id: "terminal.workspace", component: Workspace },
    { id: "terminal.settings", component: Settings },
  ],
} satisfies UiModule<PanelContext, SettingsContext>;
