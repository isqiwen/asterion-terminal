import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { registerLanguageResources } from "../i18n";
import type { ComponentType } from "react";
import type { CommandHandler } from "../bridge/commands";
import type { Snapshot } from "../bridge/client";
import type { DisplayError } from "../i18n/errors";
import type { LanguageResources } from "../i18n";

// What every workspace of a feature receives. Reads go through `query`;
// anything that acts goes through `trade`.
export type TerminalContext = {
  snapshot: Snapshot | null;
  busy: boolean;
  error: DisplayError;
  workspacePage?: string;
  workspaceParams?: Readonly<Record<string, string>>;
  navigate: (id: string, options?: { page?: string; params?: Record<string, string> }) => void;
  openSettings: (page?: "preferences" | "market" | "warehouse" | "trading") => void;
  trade: CommandHandler;
};
// A built-in feature: its workspaces and language resources.
export type TerminalFeature = {
  id: string;
  languageResources?: LanguageResources;
  workspaces: readonly {
    id: string;
    title: string;
    component: ComponentType<TerminalContext>;
    toolbar?: ComponentType<TerminalContext>;
    hideToolbar?: boolean;
  }[];
};
// Built-in registration only; React owns component lifetimes.
export function registerTerminalFeatures(features: readonly TerminalFeature[]) {
  const ids = new Set<string>();
  const workspaces = new Set<string>();
  for (const feature of features) {
    if (!feature.id || ids.has(feature.id))
      throw new Error(t("重复或无效的界面功能: {p0}", { p0: feature.id }));
    for (const workspace of feature.workspaces) {
      if (!workspace.id || workspaces.has(workspace.id))
        throw new Error(t("重复或无效的工作区: {p0}", { p0: workspace.id }));
      workspaces.add(workspace.id);
    }
    ids.add(feature.id);
  }
  for (const feature of features)
    if (feature.languageResources) registerLanguageResources(feature.id, feature.languageResources);
  return Object.freeze(
    features.map(feature =>
      Object.freeze({
        ...feature,
        workspaces: Object.freeze(
          feature.workspaces.map(
            workspace =>
              Object.freeze(
                Object.defineProperties({}, Object.getOwnPropertyDescriptors(workspace)),
              ) as TerminalFeature["workspaces"][number],
          ),
        ),
      }),
    ),
  );
}
