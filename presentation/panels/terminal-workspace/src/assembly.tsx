import { createContext, useContext, type ReactNode } from "react";
import type { UiComposition } from "@asterion/workbench/extensions/modules";
import type { DashboardWidget } from "@asterion/ui-overview/public";
import type { PanelContext } from "./context";
import type { SettingsContext } from "./settingsContext";
export type WorkspaceAssembly = {
  composition: UiComposition<PanelContext, SettingsContext>;
  dashboardWidgets: DashboardWidget[];
};
const Assembly = createContext<WorkspaceAssembly | null>(null);
export function WorkspaceAssemblyProvider({ value, children }: { value: WorkspaceAssembly; children: ReactNode }) {
  return <Assembly.Provider value={value}>{children}</Assembly.Provider>;
}
export function useWorkspaceAssembly() {
  const value = useContext(Assembly);
  if (!value) throw new Error("Workspace assembly is missing");
  return value;
}
