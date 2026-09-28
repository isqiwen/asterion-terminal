import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.overview", key, values);
import type { DashboardWidget } from "../contract";
export type { DashboardWidget } from "../contract";
export const dashboardContribution = (value: DashboardWidget) => ({
  id: value.id,
  point: "dashboard.widgets",
  value,
});
export function dashboardWidgets(
  entries: readonly {
    point: string;
    value: unknown;
  }[],
): DashboardWidget[] {
  return entries
    .filter(e => e.point === "dashboard.widgets")
    .map(e => {
      const w = e.value as DashboardWidget;
      if (
        !w ||
        typeof w.id !== "string" ||
        !w.id ||
        typeof w.title !== "string" ||
        (w.hasContent !== undefined && typeof w.hasContent !== "boolean") ||
        (w.defaultVisible !== undefined && typeof w.defaultVisible !== "boolean") ||
        typeof w.category !== "string" ||
        typeof w.description !== "string" ||
        (w.column !== undefined && !["primary", "secondary"].includes(w.column)) ||
        ![1, 2].includes(w.width) ||
        (w.position !== undefined && !["main", "aside"].includes(w.position)) ||
        typeof w.render !== "function" ||
        (w.attentionCount !== undefined && typeof w.attentionCount !== "function")
      )
        throw new Error(t("Dashboard 组件不符合当前契约"));
      return w;
    });
}
