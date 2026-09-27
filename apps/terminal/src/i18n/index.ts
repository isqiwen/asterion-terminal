import { useSyncExternalStore } from "react";
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
export type Locale = "zh-CN" | "en-US";
export type LanguageResources = Readonly<Record<Locale, Readonly<Record<string, string>>>>;
export type MessageValues = Readonly<Record<string, string | number>>;
const preference = "asterion.locale";
const resources = new Map<string, LanguageResources>([["host", { "zh-CN": zh, "en-US": en }]]);
const listeners = new Set<() => void>();
let locale: Locale = "zh-CN";
if (typeof window !== "undefined") {
  try {
    const saved = window.localStorage.getItem(preference);
    if (saved === "zh-CN" || saved === "en-US") locale = saved;
  } catch { /* Read-only storage still allows the default language to render. */ }
  window.addEventListener("storage", event => {
    if (event.key === preference && (event.newValue === "zh-CN" || event.newValue === "en-US")) {
      locale = event.newValue; publish();
    }
  });
}
function publish() {
  if (typeof document !== "undefined") document.documentElement.lang = locale;
  listeners.forEach(listener => listener());
}
export function getLocale() { return locale; }
export function setLocale(next: Locale) {
  if (next !== "zh-CN" && next !== "en-US") throw new Error("Unsupported locale");
  window.localStorage.setItem(preference, next);
  locale = next; publish();
}
export function useLocale() {
  const current = useSyncExternalStore(listener => { listeners.add(listener); return () => listeners.delete(listener); }, getLocale, () => "zh-CN" as Locale);

  return { locale: current, setLocale };
}
export function registerLanguageResources(namespace: string, catalog: LanguageResources) {
  if (!namespace || resources.has(namespace)) throw new Error(`Duplicate language namespace: ${namespace}`);
  const keys = Object.keys(catalog["zh-CN"]).sort();
  if (JSON.stringify(keys) !== JSON.stringify(Object.keys(catalog["en-US"]).sort())) throw new Error(`Language keys differ: ${namespace}`);
  for (const key of keys) {
    for (const language of ["zh-CN", "en-US"] as const) {
      const value = catalog[language][key];
      if (!value || typeof value !== "string") throw new Error(`Missing translation: ${namespace}/${key}`);
      const placeholders = (text: string) => (text.match(/\{\w+\}/g) ?? []).sort().join();
      if (placeholders(value) !== placeholders(catalog["zh-CN"][key])) throw new Error(`Translation placeholders differ: ${namespace}/${key}`);
    }
  }
  resources.set(namespace, Object.freeze({ "zh-CN": Object.freeze({ ...catalog["zh-CN"] }), "en-US": Object.freeze({ ...catalog["en-US"] }) }));
}
export function translate(namespace: string, key: string, values: MessageValues = {}): string {
  const message = resources.get(namespace)?.[locale]?.[key];
  if (message === undefined) throw new Error(`Unknown translation: ${namespace}/${key}`);
  return message.replace(/\{(\w+)\}/g, (_, name: string) => {
    if (!(name in values)) throw new Error(`Missing translation argument: ${name}`);
    return String(values[name]);
  });
}

if (typeof document !== "undefined") document.documentElement.lang = locale;

// Stored UI notices may have been created before the user switched languages.
export function localizeText(namespace: string, text: string): string | undefined {
  const catalog = resources.get(namespace);
  if (!catalog) return undefined;
  const key = Object.keys(catalog["zh-CN"]).find(key => key === text || catalog["zh-CN"][key] === text || catalog["en-US"][key] === text);
  return key === undefined ? undefined : catalog[locale][key];
}
