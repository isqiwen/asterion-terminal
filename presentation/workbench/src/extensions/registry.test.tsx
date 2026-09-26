import {
  UiComposition,
  renderSetting,
  type SettingContribution,
  type UiModule,
} from "./modules";
import { renderToStaticMarkup } from "react-dom/server";
import { expect, test } from "vitest";
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



test("static composition validates contribution ownership before rendering", () => {
  const base: UiModule<{ empty: {} }, { empty: {} }> = { id: "fixture.base" };
  const feature: UiModule<{ empty: {} }, { empty: {} }> = {
    id: "fixture.feature",
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
  const composition = new UiComposition([feature, base]);
  // Modules keep the product's listed order; there is no activation ordering.
  expect(composition.modules.map((module) => module.id)).toEqual([
    feature.id,
    base.id,
  ]);
  expect(
    renderToStaticMarkup(
      <PanelHost
        registry={composition.panels}
        activeId="fixture.report"
        context={{ empty: {} }}
      />,
    ),
  ).toContain("test-only report");
  expect(composition.settings.get("fixture.settings").title).toBe("Fixture settings");
  expect(() => new UiComposition([base, base])).toThrow("Duplicate contribution");
  expect(() => new UiComposition([{ ...base, id: "Invalid" }])).toThrow(
    "Invalid contribution ID",
  );
  expect(() => new UiComposition([base, { ...feature, panels: [] }])).toThrow(
    "Unknown contribution",
  );
  expect(new UiComposition([base]).panels.all()).toEqual([]);
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
