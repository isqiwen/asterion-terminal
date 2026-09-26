/** UI-owned scoped inputs; this plugin never imports the product or workspace. */
import type { RequestClient } from "@asterion/runtime-client/requests";
type Connection = Readonly<{ api: RequestClient; connected: boolean }>;
import type { components } from "@asterion/api-types/schema";
type Job = components["schemas"]["Job"];
type Refill = { reportId: string; jobs: Job[] } | null;
export type PanelContext = {
  research: Readonly<{
    connection: Connection;
    accountEmail: string;
    externalRefill: Refill;
    inspectData: (versionId: string, reportId: string) => void;
    submitted: (job: Job) => void;
  }>;
};
export type SettingsContext = {

};
