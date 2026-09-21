import { useSyncExternalStore } from "react";

const storageKey = "asterion.entry-language";
const changeEvent = "asterion:language-changed";
export type Language = "zh" | "en";
export type WindowKind = "main" | "settings" | "workspace" | "chart";

function readLanguage(): Language {
  return localStorage.getItem(storageKey) === "en" ? "en" : "zh";
}
function subscribe(notify: () => void) {
  const onStorage = (event: StorageEvent) => {
    if (event.key === storageKey || event.key === null) notify();
  };
  window.addEventListener("storage", onStorage);
  window.addEventListener(changeEvent, notify);
  return () => {
    window.removeEventListener("storage", onStorage);
    window.removeEventListener(changeEvent, notify);
  };
}
function setLanguage(language: Language) {
  localStorage.setItem(storageKey, language);
  window.dispatchEvent(new Event(changeEvent));
}
export function useLanguage() {
  return [useSyncExternalStore(subscribe, readLanguage), setLanguage] as const;
}
export function windowTitle(language: Language, kind: WindowKind) {
  const brand = language === "zh" ? "星枢" : "ASTERION";
  const names = {
    settings: { zh: "设置", en: "Settings" },
    workspace: { zh: "工作台", en: "Workspace" },
    chart: { zh: "历史图表", en: "Chart" },
  };
  return kind === "main" ? brand : `${names[kind][language]} · ${brand}`;
}
