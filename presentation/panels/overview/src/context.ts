/** UI-owned scoped inputs; this plugin never imports the product or workspace. */
import type { RequestClient } from "@asterion/runtime-client/requests";
import type { components } from "@asterion/api-types/schema";
type Job = components["schemas"]["Job"];
import type { DashboardWidget } from "./public";
export type PanelContext = {
  overview: Readonly<{
    marketApi: RequestClient;
    tradingApi: RequestClient;
    widgets: DashboardWidget[];
    storageKey: string;
    refresh: () => void;
    openTrading: () => void;
    openIntelligence: () => void;
    jobs: Job[];
    taskViews: readonly { id: string; title: string }[];
    connected: boolean;
    catalogError: string;
    refreshedAt: number | null;
    openData: () => void;
    openTasks: () => void;
  }>;
};
export type SettingsContext = {

};
