import type { ComponentType } from "react";
import type { Panel } from "./PanelHost";
import { ContributionRegistry } from "./registry";

export type WorkspaceContribution = {
  id: string;
  title: string;
  icon: string;
  objectTools?: boolean;
  sections: readonly { title: string; panel: string }[];
};
export type SettingContribution<C> = {
  [K in keyof C]: {
    id: string;
    scope: K;
    title: string;
    order: number;
    render: (context: C[K]) => React.ReactNode;
  };
}[keyof C];
export function renderSetting<C>(setting: SettingContribution<C>, contexts: C) {
  if (!Object.prototype.hasOwnProperty.call(contexts, setting.scope))
    throw new Error(`Missing setting scope: ${String(setting.scope)}`);
  const render = setting.render as (context: C[keyof C]) => React.ReactNode;
  return render(contexts[setting.scope]);
}
export type TerminalPlugin<C, S> = {
  apiVersion: 1;
  id: string;
  requires: readonly string[];
  workspaces?: readonly WorkspaceContribution[];
  panels?: readonly Panel<C>[];
  settings?: readonly SettingContribution<S>[];
  extensions?: readonly { id: string; point: string; value: unknown }[];
  screens?: readonly { id: string; component: ComponentType }[];
};

/** Static approved contributions. React owns mount/unmount and effect cleanup. */
export class TerminalPlugins<C, S> {
  readonly plugins: readonly TerminalPlugin<C, S>[];
  readonly workspaces: ContributionRegistry<WorkspaceContribution>;
  readonly panels: ContributionRegistry<Panel<C>>;
  readonly settings: ContributionRegistry<SettingContribution<S>>;
  readonly extensions: ContributionRegistry<{
    id: string;
    point: string;
    value: unknown;
  }>;
  readonly screens: ContributionRegistry<{
    id: string;
    component: ComponentType;
  }>;
  constructor(plugins: readonly TerminalPlugin<C, S>[]) {
    for (const plugin of plugins)
      if (plugin.apiVersion !== 1)
        throw new Error(`Unsupported plugin contract: ${plugin.id}`);
    const registry = new ContributionRegistry(plugins);
    const visiting = new Set<string>();
    const visited = new Set<string>();
    const ordered: TerminalPlugin<C, S>[] = [];
    const visit = (id: string) => {
      if (visiting.has(id)) throw new Error(`Plugin dependency cycle: ${id}`);
      if (visited.has(id)) return;
      const plugin = registry.get(id);
      visiting.add(id);
      plugin.requires.forEach(visit);
      visiting.delete(id);
      visited.add(id);
      ordered.push(plugin);
    };
    plugins.forEach((plugin) => visit(plugin.id));
    this.plugins = ordered;
    this.workspaces = new ContributionRegistry(
      plugins.flatMap((p) => p.workspaces ?? []),
    );
    this.panels = new ContributionRegistry(
      plugins.flatMap((p) => p.panels ?? []),
    );
    this.settings = new ContributionRegistry(
      plugins.flatMap((p) => p.settings ?? []),
    );
    this.extensions = new ContributionRegistry(
      plugins.flatMap((p) => p.extensions ?? []),
    );
    this.screens = new ContributionRegistry(
      plugins.flatMap((p) => p.screens ?? []),
    );
    for (const workspace of this.workspaces.all()) {
      if (workspace.objectTools !== undefined && typeof workspace.objectTools !== "boolean")
        throw new Error(`Invalid workspace object tools: ${workspace.id}`);
      if (!workspace.sections.length)
        throw new Error(`Workspace has no sections: ${workspace.id}`);
      for (const section of workspace.sections) this.panels.get(section.panel);
    }
  }
}
