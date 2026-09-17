import { useEffect, useState } from "react";

export const settingsCategories = [
  "外观",
  "安全",
  "数据源",
  "本机服务",
  "关于",
] as const;
export type SettingsCategory = (typeof settingsCategories)[number];
const key = "asterion.settings.category";
const event = "asterion:settings-category";

function readCategory(): SettingsCategory {
  const value = localStorage.getItem(key);
  return settingsCategories.find((category) => category === value) ?? "外观";
}

export function selectSettingsCategory(category: SettingsCategory) {
  localStorage.setItem(key, category);
  window.dispatchEvent(new Event(event));
}

export function useSettingsCategory() {
  const [category, setCategory] = useState(readCategory);
  useEffect(() => {
    const refresh = () => setCategory(readCategory());
    const storage = (change: StorageEvent) => {
      if (change.key === key || change.key === null) refresh();
    };
    window.addEventListener("storage", storage);
    window.addEventListener(event, refresh);
    // Recover requests issued while this window was mounting.
    refresh();
    return () => {
      window.removeEventListener("storage", storage);
      window.removeEventListener(event, refresh);
    };
  }, []);
  return [category, selectSettingsCategory] as const;
}
