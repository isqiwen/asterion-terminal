import { dashboardWidgets } from "./plugins/overview/public";
/** Default product assembly. Mechanism modules never select concrete features. */
import { TerminalPlugins } from "./extensions/plugins";
import { plugin as identity } from "./plugins/identity/plugin";
import { plugin as tasks } from "./plugins/tasks/plugin";
import { plugin as overview } from "./plugins/overview/plugin";
import { plugin as market } from "./plugins/market/plugin";
import { plugin as roles } from "./plugins/contract_roles/plugin";
import { plugin as data } from "./plugins/data/plugin";
import { plugin as tradingTime } from "./plugins/trading_time/plugin";
import { plugin as rules } from "./plugins/contract_rules/plugin";
import { plugin as research } from "./plugins/research/plugin";
import { plugin as trading } from "./plugins/trading/plugin";
import { plugin as intelligence } from "./plugins/intelligence/plugin";
import { plugin as maintenance } from "./plugins/maintenance/plugin";
import { plugin as appearance } from "./plugins/appearance/plugin";
import { plugin as workflow } from "./plugins/workflow/plugin";

import { plugin as extensions } from "./plugins/extensions/plugin";

export const distribution = new TerminalPlugins([
  identity,
  tasks,
  overview,
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
