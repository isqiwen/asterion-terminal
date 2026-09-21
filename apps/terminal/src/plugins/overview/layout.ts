export type DashboardLayout = {
  version: 1;
  refreshSeconds: 0 | 60 | 600;
  items: { id: string; width: 1 | 2; column?: "primary" | "secondary" | "full" }[];
  pulse: boolean;
  compact: boolean;
};
export function parseDashboardLayout(raw: string): DashboardLayout {
  const v = JSON.parse(raw) as DashboardLayout;
  if (!v || Object.keys(v).some(k => !["version", "refreshSeconds", "items", "pulse", "compact"].includes(k)) || v.version !== 1 || ![0,60,600].includes(v.refreshSeconds) || typeof v.pulse !== "boolean" || typeof v.compact !== "boolean" ||
    !Array.isArray(v.items) || v.items.some(i => !i || Object.keys(i).some(k => !["id", "width", "column"].includes(k)) || typeof i.id !== "string" || !i.id || ![1,2].includes(i.width) || (i.column !== undefined && !["primary", "secondary", "full"].includes(i.column))) ||
    new Set(v.items.map(i => i.id)).size !== v.items.length)
    throw new Error("总览布局不符合当前契约，原始记录已保留");
  return v;
}
