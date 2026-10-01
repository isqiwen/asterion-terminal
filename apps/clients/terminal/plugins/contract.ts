export { useWorkspaceDraft, useWorkspaceRequestId } from "../src/host/workspace/drafts";
export { Icon } from "../src/ui/Icon";
export { DatasetPicker } from "../src/ui/DatasetPicker";
export {
  CostTemplate,
  CostScheduleDetails,
  ContractCosts,
  contractCostRequest,
  saveCostTemplate,
  type ContractCostDrafts,
} from "../src/ui/CostTemplate";
import type { DisplayError } from "../src/i18n/errors";
export { ErrorNotice, asDisplayError } from "../src/i18n/errors";
export type { DisplayError } from "../src/i18n/errors";
import type { LanguageResources } from "../src/i18n";
export { translate, getLocale } from "../src/i18n";
export type { MessageValues, LanguageResources } from "../src/i18n";
import type { Snapshot, TerminalCommand } from "../src/bridge/client";
import type { ComponentType, ReactNode } from "react";

// Terminal 插件接口定义：宿主提供的能力与插件贡献的工作区、卡片。
// 契约不依赖任何具体插件；总览插件只是 DashboardWidget 的一个消费者。

// Context handed to overview cards when they render.
export type DashboardContext = Readonly<{
  widgets: DashboardWidget[];
  status?: ReactNode;
  storageKey: string;
  refresh: () => void;
  catalogError: DisplayError;
  openTasks: () => void;
}>;
export type DashboardWidget = {
  id: string;
  title: string;
  category: string;
  description: string;
  width: 1 | 2;
  defaultVisible?: boolean;
  hasContent?: boolean;
  column?: "primary" | "secondary";
  position?: "main" | "aside";
  attentionCount?: (context: DashboardContext) => number;
  render: (context: DashboardContext) => ReactNode;
};
export type TerminalContext = {
  snapshot: Snapshot | null;
  busy: boolean;
  error: DisplayError;
  widgets: DashboardWidget[];
  marketMode?: "live" | "history";
  workspacePage?: string;
  workspaceParams?: Readonly<Record<string, string>>;
  navigate: (
    id: string,
    options?: { marketMode?: "live" | "history"; page?: string; params?: Record<string, string> },
  ) => void;
  openSettings: (page?: "preferences" | "connections") => void;
  openTasks: () => void;
  refresh: () => void;
  query: (
    method:
      | "research.minutes.page"
      | "research.daily.page"
      | "research.datasets"
      | "research.coverage"
      | "market.minutes",
    params: Record<string, unknown>,
  ) => Promise<Snapshot>;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
};
// Core methods a plugin may invoke; the host rejects anything undeclared.
export type PluginCommand = TerminalCommand;
export type TerminalPlugin = {
  id: string;
  apiVersion: 1;
  // Declared capability: the host scopes context.trade to these.
  // Built-in plugins are trusted code; this prevents accidental coupling
  // between plugins, it is not a sandbox.
  commands: readonly PluginCommand[];
  languageResources?: LanguageResources;
  workspace: {
    id: string;
    title: string;
    icon: ReactNode;
    component: ComponentType<TerminalContext>;
    headerTools?: ComponentType<TerminalContext>;
    toolbar?: ComponentType<TerminalContext>;
    hideToolbar?: boolean;
  };
  tasks?: (context: TerminalContext) => TerminalTask[];
  widgets?: (context: TerminalContext) => DashboardWidget[];
};

export type TerminalTask = {
  id: string;
  title: string;
  status: string;
  completed: number;
  total: number;
  open: () => void;
};
