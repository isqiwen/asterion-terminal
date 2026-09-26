import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
import { parseViewport, type ChartViewport } from "@asterion/ui-market-panel/chartState";
import type { View } from "./navigation";
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
    view: "总览",
    inspector: false,
    tasks: false,
    taskHeight: 180,
    marketRatio: 60,
    snapshot: "",
    contract: "",
    section: "工作概览",
    linkGroup: "A",
    locked: false,
    kind: "workspace",
    owner: "",
    detached: false,
  };
  if (raw === null) return fallback;
  let value: Layout;
  try {
    value = JSON.parse(raw);
  } catch {
    throw new Error("工作区布局损坏，原始记录已保留");
  }
  if (!value || value.version !== 4)
    throw new Error("工作区布局版本不受支持，原始记录已保留");
  const strings = ["snapshot", "contract", "section", "owner"] as const;
  const flags = ["inspector", "tasks", "locked", "detached"] as const;
  if (
    typeof value.view !== "string" ||
    !value.view ||
    value.view.length > 80 ||
    !["left", "right", "top", "bottom"].includes(value.dock) ||
    !["", "A", "B", "C"].includes(value.linkGroup) ||
    !["workspace", "chart"].includes(value.kind) ||
    strings.some((key) => typeof value[key] !== "string") ||
    flags.some((key) => typeof value[key] !== "boolean") ||
    !Number.isFinite(value.marketRatio) ||
    value.marketRatio < 30 ||
    value.marketRatio > 75 ||
    !Number.isFinite(value.taskHeight) ||
    value.taskHeight < 120 ||
    value.taskHeight > 400 ||
    (value.viewport !== null && !parseViewport(value.viewport))
  )
    throw new Error("工作区布局不符合当前规范，原始记录已保留");
  return value;
}

export const layoutContract = {
  parse: parseLayout,
  editable: [
    "version",
    "viewport",
    "dock",
    "view",
    "inspector",
    "tasks",
    "taskHeight",
    "marketRatio",
    "snapshot",
    "contract",
    "section",
    "linkGroup",
    "locked",
  ] as const,
  initial(value: Layout): Layout {
    const view = new URLSearchParams(location.search).get("view");
    return !nativeDesktop && view && view.length <= 80
      ? { ...value, view }
      : value;
  },
};
