import type { IconName } from "../ui/Icon";

// Product organization is independent of plugin ownership. IDs below name the
// current contributed views, never an alternate or historical routing contract.
export const modules = [
  {
    id: "data",
    title: "数据",
    icon: "data",
    sections: [
      { id: "acquisition", title: "接入与采集", views: ["workspace.history"] },
      { id: "quality", title: "加工与质量", views: [] },
      { id: "management", title: "管理与服务", views: ["workspace.tables", "workspace.records"] },
    ],
  },
  {
    id: "intelligence",
    title: "情报",
    icon: "info",
    sections: [
      { id: "extraction", title: "信息提取", views: [] },
      { id: "verification", title: "情报核验", views: [] },
      { id: "events", title: "事件服务", views: [] },
    ],
  },
  {
    id: "analysis",
    title: "分析",
    icon: "market",
    sections: [
      { id: "exploration", title: "数据探索", views: [] },
      { id: "features", title: "特征与因子", views: ["workspace.factor"] },
      { id: "evaluation", title: "有效性评价与筛选", views: ["workspace.factor-comparison"] },
    ],
  },
  {
    id: "strategy",
    title: "策略",
    icon: "plugins",
    sections: [
      { id: "logic", title: "策略逻辑", views: [] },
      { id: "portfolio", title: "组合与仓位", views: [] },
      { id: "delivery", title: "策略管理与交付", views: [] },
    ],
  },
  {
    id: "backtest",
    title: "回测",
    icon: "backtest",
    sections: [
      { id: "experiments", title: "实验管理", views: ["workspace.backtest"] },
      { id: "simulation", title: "交易与账户模拟", views: [] },
    ],
  },
  {
    id: "evaluation",
    title: "评估",
    icon: "contract",
    sections: [
      { id: "performance", title: "绩效与归因", views: ["workspace.performance"] },
      { id: "risk", title: "风险与压力", views: [] },
      { id: "validation", title: "稳健性与综合评价", views: ["workspace.backtest-comparison"] },
    ],
  },
  {
    id: "live",
    title: "实盘",
    icon: "trading",
    sections: [
      {
        id: "operations",
        title: "准备与运行",
        views: ["workspace.market", "workspace.watchlist", "workspace.contract"],
      },
      { id: "execution", title: "订单与成交", views: ["workspace.trading"] },
      { id: "accounts", title: "账户与核对", views: [] },
    ],
  },
  {
    id: "system",
    title: "系统管理",
    icon: "settings",
    sections: [
      { id: "setup", title: "安装与配置", views: [] },
      { id: "tasks", title: "任务与节点", views: [] },
      { id: "records", title: "资料与诊断", views: [] },
    ],
  },
] satisfies {
  id: string;
  title: string;
  icon: IconName;
  sections: { id: string; title: string; views: string[] }[];
}[];
export type NavigationOptions = {
  marketMode?: "live" | "history";
  page?: string;
  params?: Record<string, string>;
};
export type NavigationState = {
  module: string;
  routes: Record<string, { section: string; view?: string }>;
  locations: Record<string, NavigationOptions>;
};
export function initialNavigation(saved?: unknown): NavigationState {
  const initial: NavigationState = { module: "data", routes: {}, locations: {} };
  if (!saved || typeof saved !== "object") return initial;
  const location = saved as Record<string, unknown>;
  const module = modules.find(item => item.id === location.module);
  const section = module?.sections.find(item => item.id === location.section);
  if (!module || !section) return initial;
  const view = typeof location.view === "string" ? location.view : undefined;
  if (section.views.length ? !view || !section.views.includes(view) : location.view !== undefined)
    return initial;
  return { ...initial, module: module.id, routes: { [module.id]: { section: section.id, view } } };
}
export function currentRoute(state: NavigationState) {
  const module = modules.find(item => item.id === state.module)!;
  const saved = state.routes[module.id];
  const section = module.sections.find(item => item.id === saved?.section) ?? module.sections[0];
  const view = saved?.view ?? section.views[0];
  return { module, section, view, options: view ? (state.locations[view] ?? {}) : {} };
}
// Persist only the destination, never object parameters or in-memory drafts.
export function navigationLocation(state: NavigationState) {
  const { module, section, view } = currentRoute(state);
  return { module: module.id, section: section.id, view };
}
export function selectModule(state: NavigationState, id: string): NavigationState {
  if (!modules.some(item => item.id === id)) throw new Error(`Unknown module: ${id}`);
  return { ...state, module: id };
}
export function selectSection(state: NavigationState, id: string): NavigationState {
  const { module } = currentRoute(state);
  const section = module.sections.find(item => item.id === id);
  if (!section) throw new Error(`Unknown section: ${id}`);
  return {
    ...state,
    routes: { ...state.routes, [module.id]: { section: id, view: section.views[0] } },
  };
}
export function selectView(
  state: NavigationState,
  id: string,
  options?: NavigationOptions,
): NavigationState {
  for (const module of modules) {
    const section = module.sections.find(item => item.views.includes(id));
    if (section)
      return {
        module: module.id,
        routes: { ...state.routes, [module.id]: { section: section.id, view: id } },
        locations: options ? { ...state.locations, [id]: options } : state.locations,
      };
  }
  throw new Error(`Unknown view: ${id}`);
}
