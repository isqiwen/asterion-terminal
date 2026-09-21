import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
import { taskView } from "./taskView";
import { RoleDiagnostics } from "./RoleDiagnostics";

export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.contract_roles",
  requires: ["asterion.identity", "asterion.tasks"],
  extensions: [{id: taskView.id, point: "tasks.views", value: taskView}],
  panels: [{
    id: "roles.diagnostics", scope: "roles",
    render: (context, active) => <RoleDiagnostics {...context} active={active} />,
  }],
};
