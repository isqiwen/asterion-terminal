import type { UiModule } from "@asterion/workbench/extensions/modules";
import type { PanelContext } from "./context";
import type { SettingsContext } from "./context";
import { taskView } from "./taskView";
import { RoleDiagnostics } from "./RoleDiagnostics";

export const uiModule = { 
  id: "asterion.ui.role-panel",
  extensions: [{id: taskView.id, point: "tasks.views", value: taskView}],
  panels: [{
    id: "roles.diagnostics", scope: "roles",
    render: (context, active) => <RoleDiagnostics {...context} active={active} />,
  }],
} satisfies UiModule<PanelContext, SettingsContext>;
