import { useEffect, useState } from "react";

export type SettingsCategory = string;
const key = "asterion.settings.category";
const event = "asterion:settings-category";

function readCategory(categories: readonly string[]): SettingsCategory {
  const value = localStorage.getItem(key);
  return categories.find((category) => category === value) ?? categories[0];
}

export function selectSettingsCategory(category: SettingsCategory) {
  localStorage.setItem(key, category);
  window.dispatchEvent(new Event(event));
}

export function useSettingsCategory(categories: readonly string[]) {
  const [category, setCategory] = useState(() => readCategory(categories));
  useEffect(() => {
    const refresh = () => setCategory(readCategory(categories));
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
