export { TaskPagination } from "../src/ui/TaskPagination";
export {
  StrategyFields,
  strategyDefaults,
  strategyOf,
  strategyRows,
  strategyRule,
  strategySides,
  type StrategyDraft,
} from "../src/ui/StrategyFields";
export { useWorkspaceDraft, useWorkspaceRequestId } from "../src/host/workspace/drafts";
export { Icon } from "../src/ui/Icon";
export { useHistoryDatasets } from "../src/ui/useHistoryDatasets";
export { DatasetPicker } from "../src/ui/DatasetPicker";
export { explicitCloseBuckets } from "../src/ui/closePolicy";
export { DataTaskAccess } from "../src/ui/DataTaskAccess";
export {
  CostTemplate,
  CostScheduleDetails,
  ContractCosts,
  contractCostRequest,
  saveCostTemplate,
  type ContractCostDrafts,
} from "../src/ui/CostTemplate";
import type { DisplayError } from "../src/i18n/errors";
export { BackendError, ErrorNotice, asDisplayError, diagnosticSummary } from "../src/i18n/errors";
export type { DisplayError } from "../src/i18n/errors";
import type { LanguageResources } from "../src/i18n";
export { translate, getLocale } from "../src/i18n";
export type { MessageValues, LanguageResources } from "../src/i18n";
import type { Snapshot, TerminalCommand } from "../src/bridge/client";
import type { ComponentType, ReactNode } from "react";

// Terminal plugins contribute workspaces, tasks and language resources.
export type TerminalContext = {
  snapshot: Snapshot | null;
  busy: boolean;
  error: DisplayError;
  marketMode?: "live" | "history";
  workspacePage?: string;
  workspaceParams?: Readonly<Record<string, string>>;
  navigate: (
    id: string,
    options?: { marketMode?: "live" | "history"; page?: string; params?: Record<string, string> },
  ) => void;
  openSettings: (page?: "preferences" | "connections" | "ctp" | "sources") => void;
  query: (
    method:
      | "data.minutes.page"
      | "data.daily.page"
      | "data.datasets"
      | "data.dataset.saved"
      | "data.coverage"
      | "data.history.usage"
      | "data.download.update.plan"
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
};

export type TerminalTask = {
  id: string;
  title: string;
  status: string;
  completed: number;
  total: number;
  open: () => void;
};
