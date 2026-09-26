import type { Language } from "@asterion/ui-kit/language";
export type WindowKind = "main" | "settings" | "workspace" | "chart";
export function windowTitle(language: Language, kind: WindowKind) {
  const brand = language === "zh" ? "星枢" : "ASTERION";
  const names = {
    settings: { zh: "设置", en: "Settings" },
    workspace: { zh: "工作台", en: "Workspace" },
    chart: { zh: "历史图表", en: "Chart" },
  };
  return kind === "main" ? brand : `${names[kind][language]} · ${brand}`;
}
