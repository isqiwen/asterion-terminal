import type { DashboardWidget } from "../overview/public";
import type { RequestClient } from "../../api/requests";
import type { Preferences } from "../appearance/preferences";
import type { Bar, Snapshot } from "../data/client";
import type { Job, TaskView } from "../tasks/public";
import type { Layout } from "./layout";

type DataTarget = { versionId: string; reportId: string } | null;
type Refill = { reportId: string; jobs: Job[] } | null;
type Connection = Readonly<{ api: RequestClient; connected: boolean }>;
export type PanelContext = {
  roles: Connection;
  trading: Connection;
  empty: Readonly<Record<string, never>>;
  overview: Readonly<{
    marketApi: RequestClient;
    tradingApi: RequestClient;
    widgets: DashboardWidget[];
    storageKey: string;
    refresh: () => void;
    openTrading: () => void;
    openIntelligence: () => void;
    jobs: Job[];
    taskViews: TaskView[];
    connected: boolean;
    catalogError: string;
    refreshedAt: number | null;
    openData: () => void;
    openTasks: () => void;
  }>;
  market: Readonly<{
    layout: Readonly<Pick<Layout, "detached" | "kind" | "dock" | "viewport">>;
    preferences: Preferences;
    bars: Bar[];
    contractBars: Bar[];
    contract: string;
    setContract: (contract: string) => void;
    selected: Snapshot | undefined;
    last: Bar | undefined;
    openData: () => void;
    setDock: (dock: Layout["dock"]) => void;
    saveViewport: (range: { from: number; to: number }) => void;
    merge: () => Promise<void>;
    popout: (kind?: "workspace" | "chart") => Promise<void>;
  }>;
  data: Readonly<{
    connection: Connection;
    select: (snapshot: Snapshot) => void;
    referenceRules: boolean;
    showReferenceRules: (value: boolean) => void;
    dataTarget: DataTarget;
    clearTarget: () => void;
    openResearch: () => void;
    openSync: () => void;
    openCatalog: () => void;
    prepared: (versionId: string, reportId: string) => void;
    submitted: (job: Job) => void;
    refilled: (jobs: Job[], reportId?: string) => void;
  }>;
  research: Readonly<{
    connection: Connection;
    accountEmail: string;
    externalRefill: Refill;
    inspectData: (versionId: string, reportId: string) => void;
    submitted: (job: Job) => void;
  }>;
};
