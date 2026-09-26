import type { PanelContext } from "@asterion/ui-terminal-workspace/context";
import type { SettingsContext } from "@asterion/ui-terminal-workspace/settingsContext";
import { uiModule as connections } from "@asterion/ui-connection-settings/module";
import { dashboardWidgets } from "@asterion/ui-overview/public";
/** Default product assembly. Mechanism modules never select concrete features. */
import { UiComposition } from "@asterion/workbench/extensions/modules";
import { uiModule as identity } from "@asterion/ui-account-settings/module";
import { uiModule as tasks } from "@asterion/ui-task-center/module";
import { uiModule as overview } from "@asterion/ui-overview/module";
import { uiModule as market } from "@asterion/ui-market-panel/module";
import { uiModule as roles } from "@asterion/ui-role-panel/module";
import { uiModule as data } from "@asterion/ui-data-panel/module";
import { uiModule as tradingTime } from "@asterion/ui-trading-time-controls/module";
import { uiModule as rules } from "@asterion/ui-market-rules-controls/module";
import { uiModule as research } from "@asterion/ui-research-panel/module";
import { uiModule as trading } from "@asterion/ui-trading-panel/module";
import { uiModule as intelligence } from "@asterion/ui-intelligence-panel/module";
import { uiModule as maintenance } from "@asterion/ui-maintenance-panel/module";
import { uiModule as appearance } from "@asterion/ui-appearance-settings/module";
import { uiModule as workflow } from "@asterion/ui-terminal-workspace/module";

import { uiModule as extensions } from "@asterion/ui-extension-settings/module";

export const distribution = new UiComposition<PanelContext, SettingsContext>([
  identity,
  tasks,
  overview,
  connections,
  market,
  data,
  roles,
  rules,
  tradingTime,
  research,
  trading,
  intelligence,
  appearance,
  maintenance,
  extensions,
  workflow,
]);
export const workspaces = distribution.workspaces.all();
export const panels = distribution.panels;

/** Default dashboard composition is a distribution choice, not host dispatch. */
const dashboardOrder = [
  "market.quotes", "trading.portfolio", "trading.risk-metrics",
  "market.heatmap", "intelligence.sentiment", "intelligence.rates", "intelligence.news",
];
export const dashboardContributions = dashboardWidgets(distribution.extensions.all()).sort((a, b) => {
  const rank = (id: string) => { const i = dashboardOrder.indexOf(id); return i < 0 ? dashboardOrder.length : i; };
  return rank(a.id) - rank(b.id);
});
