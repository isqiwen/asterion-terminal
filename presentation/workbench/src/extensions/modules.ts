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
/** One UI package's contributions. Packages are composed at build time. */
export type UiModule<C, S> = {
  id: string;
  workspaces?: readonly WorkspaceContribution[];
  panels?: readonly Panel<C>[];
  settings?: readonly SettingContribution<S>[];
  extensions?: readonly { id: string; point: string; value: unknown }[];
  screens?: readonly { id: string; component: ComponentType }[];
};

/**
 * The product's statically composed UI modules. There is no activation order,
 * dependency resolution or run-time loading: package dependencies are checked
 * at build time, and every contribution ID must be unique.
 */
export class UiComposition<C, S> {
  readonly modules: readonly UiModule<C, S>[];
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
  constructor(modules: readonly UiModule<C, S>[]) {
    new ContributionRegistry(modules);
    this.modules = modules;
    this.workspaces = new ContributionRegistry(
      modules.flatMap((m) => m.workspaces ?? []),
    );
    this.panels = new ContributionRegistry(modules.flatMap((m) => m.panels ?? []));
    this.settings = new ContributionRegistry(
      modules.flatMap((m) => m.settings ?? []),
    );
    this.extensions = new ContributionRegistry(
      modules.flatMap((m) => m.extensions ?? []),
    );
    this.screens = new ContributionRegistry(modules.flatMap((m) => m.screens ?? []));
    for (const workspace of this.workspaces.all()) {
      if (workspace.objectTools !== undefined && typeof workspace.objectTools !== "boolean")
        throw new Error(`Invalid workspace object tools: ${workspace.id}`);
      if (!workspace.sections.length)
        throw new Error(`Workspace has no sections: ${workspace.id}`);
      for (const section of workspace.sections) this.panels.get(section.panel);
    }
  }
}
