import { expect, test } from "vitest";
import { distribution, panels, workspaces } from "./distribution";
import { createCommands, type CommandContext } from "@asterion/ui-terminal-workspace/commands";
import { dashboardWidgets } from "@asterion/ui-overview/public";
test("built-in navigation resolves panels and commands with lock and chart constraints", async () => {
  const commands = createCommands(workspaces);
  const selected: string[] = [];
  const context: CommandContext = {
    locked: false,
    chartWindow: true,
    setView: (view) => {
      selected.push(view);
    },
    toggleTasks: () => {
      selected.push("tasks");
    },
    openSettings: async () => {
      selected.push("settings");
    },
  };
  for (const workspace of workspaces) {
    expect(commands.get(workspace.id).title).toBe(workspace.title);
    for (const section of workspace.sections)
      expect(panels.get(section.panel)).toBeDefined();
  }
  expect(await commands.execute("workspace.data", context)).toBe(false);
  expect(await commands.execute("workspace.market", context)).toBe(true);
  expect(
    await commands.execute("terminal.tasks", { ...context, locked: true }),
  ).toBe(false);
  expect(selected).toEqual(["市场"]);
});

test("default distribution contributes all dashboard domains with unique identities", () => {
  const widgets = dashboardWidgets(distribution.extensions.all());
  expect(new Set(widgets.map(w => w.id)).size).toBe(widgets.length);
  expect(widgets.map(w => w.id)).toEqual(expect.arrayContaining(["trading.portfolio", "trading.risk-metrics", "market.quotes", "market.heatmap", "intelligence.news", "intelligence.sentiment", "intelligence.rates", "tasks.pulse"]));
  expect(() => dashboardWidgets([{ point: "dashboard.widgets", value: { id: "broken" } }])).toThrow();
});
