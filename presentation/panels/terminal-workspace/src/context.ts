import type { PanelContext as DataPanelContext } from "@asterion/ui-data-panel/context";
import type { PanelContext as OverviewContext } from "@asterion/ui-overview/context";
import type { PanelContext as MarketPanelContext } from "@asterion/ui-market-panel/context";
import type { PanelContext as ResearchPanelContext } from "@asterion/ui-research-panel/context";
import type { PanelContext as TradingPanelContext } from "@asterion/ui-trading-panel/context";
import type { PanelContext as RolePanelContext } from "@asterion/ui-role-panel/context";
import type { PanelContext as IntelligencePanelContext } from "@asterion/ui-intelligence-panel/context";
export type PanelContext = DataPanelContext & OverviewContext & MarketPanelContext & ResearchPanelContext & TradingPanelContext & RolePanelContext & IntelligencePanelContext;
