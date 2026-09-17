import { parseViewport, type ChartViewport } from "./chartState";
export const views = ["总览", "市场", "数据", "研究", "交易", "情报"] as const;
export type View = (typeof views)[number];
export type Layout = {
  version: 4;
  viewport: ChartViewport | null;
  dock: "left" | "right" | "top" | "bottom";
  view: View;
  inspector: boolean;
  tasks: boolean;
  taskHeight: number;
  marketRatio: number;
  snapshot: string;
  contract: string;
  section: string;
  linkGroup: string;
  locked: boolean;
  kind: "workspace" | "chart";
  owner: string;
  detached: boolean;
};
export function parseLayout(raw: string | null): Layout {
  const fallback: Layout = {
    version: 4,
    viewport: null,
    dock: "left",
    view: "市场",
    inspector: false,
    tasks: false,
    taskHeight: 180,
    marketRatio: 60,
    snapshot: "",
    contract: "",
    section: "行情全景",
    linkGroup: "A",
    locked: false,
    kind: "workspace",
    owner: "",
    detached: false,
  };
  try {
    const value = JSON.parse(raw ?? "null");
    if (value?.version !== 4) return fallback;
    return {
      ...fallback,
      viewport: parseViewport(value.viewport),
      dock: ["left", "right", "top", "bottom"].includes(value.dock)
        ? value.dock
        : "left",
      marketRatio:
        typeof value.marketRatio === "number" &&
        Number.isFinite(value.marketRatio)
          ? Math.max(30, Math.min(75, value.marketRatio))
          : 60,
      snapshot: typeof value.snapshot === "string" ? value.snapshot : "",
      contract: typeof value.contract === "string" ? value.contract : "",
      section: typeof value.section === "string" ? value.section : "行情全景",
      linkGroup: ["", "A", "B", "C"].includes(value.linkGroup)
        ? value.linkGroup
        : "A",
      locked: value.locked === true,
      kind: value.kind === "chart" ? "chart" : "workspace",
      owner: typeof value.owner === "string" ? value.owner : "",
      detached: value.detached === true,
      view: views.includes(value.view) ? value.view : fallback.view,
      inspector: typeof value.inspector === "boolean" ? value.inspector : false,
      tasks: typeof value.tasks === "boolean" ? value.tasks : false,
      taskHeight:
        typeof value.taskHeight === "number" &&
        Number.isFinite(value.taskHeight)
          ? Math.max(120, Math.min(400, value.taskHeight))
          : 180,
    };
  } catch {
    return fallback;
  }
}
