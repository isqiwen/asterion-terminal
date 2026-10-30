export type QueryCommand =
  | "research.logic.list"
  | "research.logic.read"
  | "research.logic.examples"
  | "research.logic.example.read"
  | "research.strategy.list"
  | "research.strategy.read"
  | "research.strategy.delivery.read"
  | "research.strategy.delivery.list"
  | "research.variable.check"
  | "research.variable.read"
  | "research.variable.list"
  | "live.facts.status"
  | "live.facts.read"
  | "resource.catalogue"
  | "resource.inspect"
  | "task.result.read"
  | "backtest.budget.review"
  | "backtest.margins.preview"
  | "task.metadata.read"
  | "task.retirement.preview"
  | "task.evidence.export"
  | "evaluation.report.export"
  | "task.comparison.export"
  | "task.quantstats.analyze"
  | "task.portfolio.analyze"
  | "data.minutes.page"
  | "data.minutes.range"
  | "data.minutes.evidence"
  | "data.datasets"
  | "data.dataset.saved"
  | "data.coverage"
  | "data.history.usage"
  | "data.history.retirement.preview"
  | "data.tables.retirement.preview"
  | "data.tables.retirements"
  | "data.history.retirements"
  | "data.download.update.plan"
  | "data.tables.refresh.plan"
  | "data.tables.coverage"
  | "data.tables.page"
  | "data.tables.products"
  | "data.tables.listed_contracts"
  | "data.tables.daily"
  | "data.tables.contracts"
  | "market.minutes"
  | "market.observation.preview"
  | "data.observation.read"
  | "data.material.sources"
  | "data.material.task"
  | "data.material.read";
import type { CommandHandler } from "../src/bridge/commands";
export { TaskPagination } from "../src/ui/TaskPagination";
export {
  StrategyFields,
  strategyDefaults,
  strategiesOf,
  strategyRows,
  strategyRule,
  strategySides,
  strategySize,
  type StrategyDraft,
} from "../src/ui/StrategyFields";
export {
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useWorkspaceUnsaved,
} from "../src/host/workspace/drafts";
export { save, nativeDesktop } from "../src/bridge/desktop";
export { lab } from "../src/bridge/lab";
export type {
  Coverage as LabCoverage,
  Summary as LabSummary,
  Task as LabTask,
  SourceDescriptor as LabSource,
  SourceTable as LabSourceTable,
} from "../src/generated/asterion/lab_pb";
export { Icon } from "../src/ui/Icon";
export { useHistoryDatasets } from "../src/ui/useHistoryDatasets";
export { dailyTableOf, useDailyTable } from "../src/ui/useDailyTable";
export { DatasetPicker } from "../src/ui/DatasetPicker";
export { RollSchedule } from "../src/ui/RollSchedule";
export { multipliers, priceIncrements } from "../src/ui/contractUnits";
export { explicitCloseBuckets } from "../src/ui/closePolicy";
export { WorkloadAccess } from "../src/ui/WorkloadAccess";
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
import type { ComponentType } from "react";

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
  query: CommandHandler<Snapshot, QueryCommand>;
  trade: CommandHandler;
};
// Core methods a plugin may invoke; the host rejects anything undeclared.
export type PluginCommand = TerminalCommand;
export type TerminalPlugin = {
  id: string;
  // Declared capability: the host scopes context.trade to these.
  // Built-in plugins are trusted code; this prevents accidental coupling
  // between plugins, it is not a sandbox.
  commands: readonly PluginCommand[];
  languageResources?: LanguageResources;
  workspaces: readonly {
    id: string;
    title: string;
    component: ComponentType<TerminalContext>;
    headerTools?: ComponentType<TerminalContext>;
    toolbar?: ComponentType<TerminalContext>;
    hideToolbar?: boolean;
  }[];
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
