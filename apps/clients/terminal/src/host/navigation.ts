import type { IconName } from "../ui/Icon";

// Product organization is independent of plugin ownership. Each module lists
// its views in tab order; IDs name the current contributed views. Hidden
// modules have no entry of their own: the old research views stay reachable
// from their tasks until the old research service is deleted (M2.6).
export const modules = [
  {
    id: "market",
    title: "行情",
    icon: "market",
    views: ["workspace.market", "workspace.watchlist", "workspace.contract"],
  },
  { id: "research", title: "研究", icon: "backtest", views: ["workspace.lab-research"] },
  {
    id: "live",
    title: "交易",
    icon: "trading",
    views: ["workspace.trading"],
  },
  {
    id: "data",
    title: "数据",
    icon: "data",
    views: [
      "workspace.history",
      "workspace.coverage",
      "workspace.tables",
      "workspace.records",
      "workspace.materials",
    ],
  },
  {
    id: "analysis",
    title: "分析",
    icon: "info",
    hidden: true,
    views: [
      "workspace.factor",
      "workspace.factor-comparison",
      "workspace.research-samples",
      "workspace.research-statistics",
      "workspace.research-variables",
    ],
  },
  {
    id: "strategy",
    title: "策略",
    icon: "plugins",
    hidden: true,
    views: ["workspace.research-logic", "workspace.strategy-packages"],
  },
  { id: "backtest", title: "回测", icon: "backtest", hidden: true, views: ["workspace.backtest"] },
  {
    id: "evaluation",
    title: "评估",
    icon: "contract",
    hidden: true,
    views: ["workspace.performance", "workspace.risk", "workspace.backtest-comparison"],
  },
] satisfies { id: string; title: string; icon: IconName; views: string[]; hidden?: boolean }[];
// The modules the top bar offers, in order.
export const topModules = modules.filter(module => !("hidden" in module && module.hidden));
export type NavigationOptions = {
  marketMode?: "live" | "history";
  page?: string;
  params?: Record<string, string>;
};
export type NavigationState = {
  module: string;
  // The view last shown in each module.
  routes: Record<string, string>;
  locations: Record<string, NavigationOptions>;
};
export function initialNavigation(saved?: unknown): NavigationState {
  const initial: NavigationState = { module: "data", routes: {}, locations: {} };
  if (!saved || typeof saved !== "object") return initial;
  const location = saved as Record<string, unknown>;
  const module = modules.find(item => item.id === location.module);
  const view = location.view;
  if (!module || typeof view !== "string" || !module.views.includes(view)) return initial;
  return { ...initial, module: module.id, routes: { [module.id]: view } };
}
export function currentRoute(state: NavigationState) {
  const module = modules.find(item => item.id === state.module)!;
  const view = state.routes[module.id] ?? module.views[0];
  return { module, view, options: state.locations[view] ?? {} };
}
// Persist only the destination, never object parameters or in-memory drafts.
export function navigationLocation(state: NavigationState) {
  const { module, view } = currentRoute(state);
  return { module: module.id, view };
}
export function selectModule(state: NavigationState, id: string): NavigationState {
  if (!modules.some(item => item.id === id)) throw new Error(`Unknown module: ${id}`);
  return { ...state, module: id };
}
export function selectView(
  state: NavigationState,
  id: string,
  options?: NavigationOptions,
): NavigationState {
  const module = modules.find(item => (item.views as string[]).includes(id));
  if (!module) throw new Error(`Unknown view: ${id}`);
  return {
    module: module.id,
    routes: { ...state.routes, [module.id]: id },
    locations: options ? { ...state.locations, [id]: options } : state.locations,
  };
}
