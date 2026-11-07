// Reads a feature may make through `query`; anything that acts goes through `trade`.
import type { CommandHandler } from "../src/bridge/commands";
export { useWorkspaceDraft } from "../src/host/workspace/drafts";
export { save } from "../src/bridge/desktop";
export { lab } from "../src/bridge/lab";
export { warehouseData } from "../src/bridge/warehouse-data";
export { marketData } from "../src/bridge/market-data";
export type {
  AcquisitionOptions,
  AcquisitionTask as MarketTask,
  DatasetSummary as MarketSummary,
  DatasetCoverage as MarketCoverage,
  DatasetChange as MarketChange,
} from "../src/generated/asterion/warehouse_pb";
export type { MinuteBar } from "../src/generated/asterion/market_pb";
export { explicitCloseBuckets } from "../src/ui/closePolicy";
export { protocolPeriod, type ChartPeriod } from "../src/ui/periods";
export type { DatasetSummary as LabSummary } from "../src/generated/asterion/warehouse_pb";
export type {
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
export { catalogName, contractName, identityName, productLabel } from "../src/ui/contract-names";
export type { MessageValues, LanguageResources } from "../src/i18n";
import type { Snapshot } from "../src/bridge/client";
import type { ComponentType } from "react";

// Built-in Terminal features provide workspaces and language resources.
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
