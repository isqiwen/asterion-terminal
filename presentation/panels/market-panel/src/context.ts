/** UI-owned scoped inputs; this plugin never imports the product or workspace. */
import type { Preferences } from "@asterion/ui-kit/preferences";
import type { Bar, Snapshot } from "@asterion/ui-data-panel/client";
import type { ChartViewport } from "./chartState";
type MarketLayout = { detached: boolean; kind: "workspace" | "chart"; dock: "left" | "right" | "top" | "bottom"; viewport: ChartViewport | null };
export type PanelContext = {
  market: Readonly<{
    layout: Readonly<MarketLayout>;
    preferences: Preferences;
    bars: Bar[];
    contractBars: Bar[];
    contract: string;
    setContract: (contract: string) => void;
    selected: Snapshot | undefined;
    last: Bar | undefined;
    openData: () => void;
    setDock: (dock: MarketLayout["dock"]) => void;
    saveViewport: (range: { from: number; to: number }) => void;
    merge: () => Promise<void>;
    popout: (kind?: "workspace" | "chart") => Promise<void>;
  }>;
};
export type SettingsContext = {

};
