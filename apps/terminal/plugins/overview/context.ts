import type { DashboardWidget } from "./public";
export type PanelContext = {
  overview: Readonly<{
    widgets: DashboardWidget[];
    status?: import("react").ReactNode;
    storageKey: string;
    refresh: () => void;
    catalogError: import("../contract").DisplayError;
    openTasks: () => void;
  }>;
};
