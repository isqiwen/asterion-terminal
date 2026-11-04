// Reads a feature may make through `query`; anything that acts goes through `trade`.
import type { CommandHandler } from "../src/bridge/commands";
export { useWorkspaceDraft } from "../src/host/workspace/drafts";
export { save } from "../src/bridge/desktop";
export { lab } from "../src/bridge/lab";
export { marketData } from "../src/bridge/market-data";
export type {
  Source as MarketSource,
  Task as MarketTask,
} from "../src/generated/asterion/acquisition_pb";
export type {
  Summary as MarketSummary,
  Coverage as MarketCoverage,
  Comparison as MinuteComparisonResult,
  Differences as MinuteDifferences,
  MinuteValues,
} from "../src/generated/asterion/dataset_pb";
export { explicitCloseBuckets } from "../src/ui/closePolicy";
export type {
  Summary as LabSummary,
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
export { notify } from "../src/host/notifications";
export { ActionError, useActionError } from "../src/host/action-error";
export { MarketUnset } from "../src/host/components/MarketUnset";
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
  openSettings: (page?: "preferences" | "connections" | "trading" | "sources") => void;
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
