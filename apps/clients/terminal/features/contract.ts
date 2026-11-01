// Reads a feature may make through `query`; anything that acts goes through `trade`.
export type QueryCommand = "market.minutes";
import type { CommandHandler } from "../src/bridge/commands";
export { useWorkspaceDraft } from "../src/host/workspace/drafts";
export { save } from "../src/bridge/desktop";
export { lab } from "../src/bridge/lab";
export { explicitCloseBuckets } from "../src/ui/closePolicy";
export type {
  Coverage as LabCoverage,
  Summary as LabSummary,
  Task as LabTask,
  SourceDescriptor as LabSource,
  SourceTable as LabSourceTable,
  RunDetail as LabRunDetail,
  RunReport as LabRunReport,
  RunSummary as LabRunSummary,
  Strategies as LabStrategies,
  FactorResult as LabFactorResult,
  ChartBars as LabChartBars,
  StoredContract as LabStoredContract,
} from "../src/generated/asterion/lab_pb";
import type { DisplayError } from "../src/i18n/errors";
export { BackendError, ErrorNotice, asDisplayError } from "../src/i18n/errors";
export type { DisplayError } from "../src/i18n/errors";
import type { LanguageResources } from "../src/i18n";
export { translate, getLocale } from "../src/i18n";
export type { MessageValues, LanguageResources } from "../src/i18n";
import type { Snapshot } from "../src/bridge/client";
import type { ComponentType } from "react";

// Built-in Terminal features provide workspaces and language resources.
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
  openSettings: (page?: "preferences" | "connections" | "trading" | "sources" | "research") => void;
  query: CommandHandler<Snapshot, QueryCommand>;
  trade: CommandHandler;
};
// Built-in application features share the typed application context.
export type TerminalFeature = {
  id: string;
  languageResources?: LanguageResources;
  workspaces: readonly {
    id: string;
    title: string;
    component: ComponentType<TerminalContext>;
    headerTools?: ComponentType<TerminalContext>;
    toolbar?: ComponentType<TerminalContext>;
    hideToolbar?: boolean;
  }[];
};
