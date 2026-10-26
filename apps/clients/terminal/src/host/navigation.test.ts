import { expect, test } from "vitest";
import {
  modules,
  initialNavigation,
  currentRoute,
  navigationLocation,
  selectModule,
  selectView,
} from "./navigation";
import { workspaces } from "../plugins";
import { setLocale, translate } from "../i18n";

test("product catalog has the required modules and places every contributed view exactly once", () => {
  expect(modules.map(module => module.id)).toEqual([
    "market",
    "data",
    "analysis",
    "strategy",
    "backtest",
    "evaluation",
    "live",
  ]);
  const views = modules.flatMap(module => module.views);
  expect(new Set(views).size).toBe(views.length);
  expect([...views].sort()).toEqual(workspaces.map(view => view.id).sort());
  for (const locale of ["zh-CN", "en-US"] as const) {
    setLocale(locale);
    for (const module of modules) expect(translate("host", module.title)).toBeTruthy();
    for (const view of workspaces) expect(view.title).toBeTruthy();
  }
  setLocale("zh-CN");
});

test("first launch opens data and module switching retains each view's location", () => {
  let state = initialNavigation();
  expect(currentRoute(state).view).toBe("workspace.history");
  state = selectView(state, "workspace.factor", { page: "task", params: { id: "factor-1" } });
  state = selectView(state, "workspace.backtest", { page: "task", params: { id: "backtest-1" } });
  state = selectModule(state, "analysis");
  expect(currentRoute(state)).toMatchObject({
    view: "workspace.factor",
    options: { params: { id: "factor-1" } },
  });
  state = selectModule(state, "backtest");
  expect(currentRoute(state).options.params?.id).toBe("backtest-1");
});

test("market, watchlist and contract routes retain independent parameters", () => {
  let state = selectView(initialNavigation(), "workspace.market", { marketMode: "history" });
  state = selectView(state, "workspace.contract", { params: { symbol: "rb2610" } });
  state = selectView(state, "workspace.watchlist");
  expect(currentRoute(state).options).toEqual({});
  state = selectView(state, "workspace.market");
  expect(currentRoute(state).options.marketMode).toBe("history");
  expect(() => selectView(state, "unknown")).toThrow("Unknown view");
});

test("reload restores the concrete destination without persisting object parameters", () => {
  const state = selectView(initialNavigation(), "workspace.trading", {
    params: { account: "private" },
  });
  const location = navigationLocation(state);
  expect(location).toEqual({ module: "live", view: "workspace.trading" });
  const restored = initialNavigation(JSON.parse(JSON.stringify(location)));
  expect(currentRoute(restored).view).toBe("workspace.trading");
  expect(restored.locations).toEqual({});
  expect(initialNavigation({ ...location, view: "workspace.factor" }).module).toBe("data");
  const pending = selectView(selectModule(state, "evaluation"), "workspace.risk");
  expect(currentRoute(initialNavigation(navigationLocation(pending))).view).toBe("workspace.risk");
});
