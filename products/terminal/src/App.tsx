import { ApplicationHost } from "@asterion/workbench/extensions/ApplicationHost";
import { distribution, dashboardContributions } from "./distribution";
import "@asterion/ui-kit/theme/style.css";
import { Frame } from "@asterion/ui-terminal-workspace/Frame";
import { SetupGate } from "@asterion/ui-terminal-workspace/SetupGate";
import { WorkspaceAssemblyProvider } from "@asterion/ui-terminal-workspace/assembly";
const assembly = { composition: distribution, dashboardWidgets: dashboardContributions };
export function Terminal() {
  return <WorkspaceAssemblyProvider value={assembly}><SetupGate><Frame>
    <ApplicationHost composition={distribution} screen={new URLSearchParams(location.search).get("screen") === "settings" ? "terminal.settings" : "terminal.workspace"} />
  </Frame></SetupGate></WorkspaceAssemblyProvider>;
}
