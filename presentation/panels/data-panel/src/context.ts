/** UI-owned scoped inputs; this plugin never imports the product or workspace. */
import type { RequestClient } from "@asterion/runtime-client/requests";
type Connection = Readonly<{ api: RequestClient; connected: boolean }>;
import type { components } from "@asterion/api-types/schema";
type Job = components["schemas"]["Job"];
import type { Snapshot } from "./client";
type DataTarget = { versionId: string; reportId: string } | null;
export type PanelContext = {
  data: Readonly<{
    connection: Connection;
    rolesConnection: Connection;
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
};
export type SettingsContext = {
  data: Readonly<{ api: RequestClient }>;
};
