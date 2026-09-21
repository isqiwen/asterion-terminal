import {
  TerminalPlugins,
  renderSetting,
  type SettingContribution,
  type TerminalPlugin,
} from "./plugins";
import { renderToStaticMarkup } from "react-dom/server";
import { expect, test } from "vitest";
import { panels, workspaces } from "../distribution";
import {
  createCommands,
  type CommandContext,
} from "../plugins/workflow/commands";
import { PanelHost, type Panel } from "./PanelHost";
import { CommandRegistry, ContributionRegistry } from "./registry";

test("test-only contributions execute and render through generic hosts", async () => {
  const output: string[] = [];
  const context = { allowed: true, output };
  const registry = new CommandRegistry<typeof context>([
    {
      id: "fixture.report",
      title: "测试报告",
      key: "r",
      enabled: (value) => value.allowed,
      execute: (value) => {
        value.output.push("computed");
      },
    },
  ]);
  const command = registry.shortcut("R")!;
  expect(
    await registry.execute(command.id, { ...context, allowed: false }),
  ).toBe(false);
  expect(output).toEqual([]);
  expect(await registry.execute(command.id, context)).toBe(true);
  const testPanels = new ContributionRegistry<
    Panel<{ report: typeof context }>
  >([
    {
      id: "fixture.result",
      scope: "report",
      render: (value) => <output>{value.output.join(",")}</output>,
    },
  ]);
  expect(
    renderToStaticMarkup(
      <PanelHost
        registry={testPanels}
        activeId="fixture.result"
        context={{ report: context }}
      />,
    ),
  ).toContain("computed");
  expect(() => testPanels.get("fixture.missing")).toThrow(
    "Unknown contribution",
  );
  expect(
    () => new ContributionRegistry([...testPanels.all(), ...testPanels.all()]),
  ).toThrow("Duplicate contribution");
  expect(
    () => new CommandRegistry([command, { ...command, id: "fixture.other" }]),
  ).toThrow("Duplicate command shortcut");
  expect(() => new ContributionRegistry([{ id: "" }])).toThrow(
    "Invalid contribution ID",
  );
});

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

test("plugin dependency graph and contribution ownership are validated before rendering", () => {
  const base: TerminalPlugin<{ empty: {} }, { empty: {} }> = {
    apiVersion: 1,
    id: "fixture.base",
    requires: [],
  };
  const feature: TerminalPlugin<{ empty: {} }, { empty: {} }> = {
    apiVersion: 1,
    id: "fixture.feature",
    requires: [base.id],
    workspaces: [
      {
        id: "fixture.workspace",
        title: "Fixture",
        icon: "F",
        sections: [{ title: "Report", panel: "fixture.report" }],
      },
    ],
    panels: [
      {
        id: "fixture.report",
        scope: "empty",
        render: () => <output>test-only report</output>,
      },
    ],
    settings: [
      {
        id: "fixture.settings",
        scope: "empty",
        title: "Fixture settings",
        order: 0,
        render: () => <input aria-label="Fixture value" />,
      },
    ],
  };
  const host = new TerminalPlugins([feature, base]);
  expect(host.plugins.map((plugin) => plugin.id)).toEqual([
    base.id,
    feature.id,
  ]);
  expect(
    renderToStaticMarkup(
      <PanelHost
        registry={host.panels}
        activeId="fixture.report"
        context={{ empty: {} }}
      />,
    ),
  ).toContain("test-only report");
  expect(host.settings.get("fixture.settings").title).toBe("Fixture settings");
  expect(() => new TerminalPlugins([feature])).toThrow("Unknown contribution");
  expect(() => new TerminalPlugins([base, base])).toThrow(
    "Duplicate contribution",
  );
  expect(() => new TerminalPlugins([{ ...base, requires: [base.id] }])).toThrow(
    "dependency cycle",
  );
  expect(
    () =>
      new TerminalPlugins([
        { ...base, apiVersion: -1 } as unknown as TerminalPlugin<
          { empty: {} },
          { empty: {} }
        >,
      ]),
  ).toThrow("Unsupported plugin contract");
  expect(() => new TerminalPlugins([base, { ...feature, panels: [] }])).toThrow(
    "Unknown contribution",
  );
  expect(new TerminalPlugins([base]).panels.all()).toEqual([]);
});

test("panel host passes only the selected scope and rejects missing scopes", () => {
  type Contexts = { chart: { title: string }; service: { token: string } };
  let received: unknown;
  const registry = new ContributionRegistry<Panel<Contexts>>([
    {
      id: "fixture.chart",
      scope: "chart",
      render: (value) => {
        received = value;
        return <output>{value.title}</output>;
      },
    },
  ]);
  const context: Contexts = {
    chart: { title: "chart" },
    service: { token: "private" },
  };
  expect(
    renderToStaticMarkup(
      <PanelHost
        registry={registry}
        activeId="fixture.chart"
        context={context}
      />,
    ),
  ).toContain("chart");
  expect(received).toEqual({ title: "chart" });
  expect(() =>
    renderToStaticMarkup(
      <PanelHost
        registry={registry}
        activeId="fixture.chart"
        context={{ service: context.service } as Contexts}
      />,
    ),
  ).toThrow("Missing panel scope");
});

test("settings render with scoped operations without receiving other resources", () => {
  type Contexts = {
    appearance: { density: string };
    service: { token: string };
  };
  let received: unknown;
  const setting: SettingContribution<Contexts> = {
    id: "fixture.appearance",
    scope: "appearance",
    title: "Appearance",
    order: 0,
    render: (context) => {
      received = context;
      return context.density;
    },
  };
  expect(
    renderSetting(setting, {
      appearance: { density: "compact" },
      service: { token: "private" },
    }),
  ).toBe("compact");
  expect(received).toEqual({ density: "compact" });
  expect(() => renderSetting(setting, {} as Contexts)).toThrow(
    "Missing setting scope",
  );
});
