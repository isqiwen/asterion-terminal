import { tushareDaily } from "./tushare-daily";
import { tushareMinutes } from "./tushare-minutes";
import type {
  HistoryContractCatalog,
  Snapshot,
  ResearchResult,
  ResearchTask,
  TerminalCommand,
} from "../../src/bridge/client";

// Data-workbench extension contract. Provider rules stay beside their adapter;
// the host and shared page do not infer capabilities from provider names.
export type HistorySource = {
  id: string;
  name: string;
  asset: string;
  dataType: string;
  description: string;
  catalogCommand: TerminalCommand;
  exchanges: readonly string[];
  catalog: (snapshot: Snapshot | null) => HistoryContractCatalog | null;
  credential: { label: string; required: boolean; maxLength: number; help: string };
  intervals: readonly (number | "day")[];
  timeAxis: "instant" | "trading-day";
  timezone: string;
  rate: { default: number; max: number };
  command: TerminalCommand;
  parameters: (
    query: HistoryQuery,
    credential: string,
    catalog: HistoryContractCatalog,
  ) => Record<string, unknown>;
  ownsTask: (task: ResearchTask) => boolean;
  dataset: (result: ResearchResult) => HistoryDataset | null;
};
export type HistoryQuery = {
  code: string;
  frequency: string;
  exchange: string;
  product: string;
  rate: string;
};
export type HistoryDataset = {
  contract: string;
  interval: number | "day";
  begin: string;
  end: string;
  rows: number;
  directory: string;
  digest: string;
};
export const historySources: readonly HistorySource[] = [tushareMinutes, tushareDaily];
