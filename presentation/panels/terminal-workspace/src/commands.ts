import type { WorkspaceContribution } from "@asterion/workbench/extensions/modules";
import { CommandRegistry } from "@asterion/workbench/extensions/registry";
import type { View } from "./navigation";

export type CommandContext = {
  locked: boolean;
  chartWindow: boolean;
  setView: (view: View) => void;
  toggleTasks: () => void;
  openSettings: () => Promise<void>;
};
export function createCommands(workspaces: readonly WorkspaceContribution[]) {
  return new CommandRegistry<CommandContext>([
    ...workspaces.map((workspace, index) => ({
      id: workspace.id,
      title: workspace.title,
      key: String(index + 1),
      enabled: (context: CommandContext) =>
        !context.locked && (!context.chartWindow || workspace.title === "市场"),
      execute: (context: CommandContext) => context.setView(workspace.title),
    })),
    {
      id: "terminal.tasks",
      title: "任务",
      key: "j",
      enabled: (context) => !context.locked,
      execute: (context) => context.toggleTasks(),
    },
    {
      id: "terminal.settings",
      title: "设置",
      key: ",",
      enabled: (context) => !context.locked,
      execute: (context) => context.openSettings(),
    },
  ]);
}
