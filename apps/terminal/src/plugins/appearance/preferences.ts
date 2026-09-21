import { useEffect, useState } from "react";
export type Preferences = {
  density: "compact" | "comfortable";
  colors: "china" | "international";
};
const key = "asterion.preferences";
export function readPreferences(): Preferences {
  try {
    const p = JSON.parse(localStorage.getItem(key) ?? "{}");
    return {
      density: p.density === "comfortable" ? "comfortable" : "compact",
      colors: p.colors === "international" ? "international" : "china",
    };
  } catch {
    return { density: "compact", colors: "china" };
  }
}
export function savePreferences(preferences: Preferences) {
  localStorage.setItem(key, JSON.stringify(preferences));
  window.dispatchEvent(new Event("asterion-preferences"));
}
export function usePreferences() {
  const [preferences, setPreferences] = useState(readPreferences);
  useEffect(() => {
    const update = () => setPreferences(readPreferences());
    window.addEventListener("storage", update);
    window.addEventListener("asterion-preferences", update);
    return () => {
      window.removeEventListener("storage", update);
      window.removeEventListener("asterion-preferences", update);
    };
  }, []);
  useEffect(() => {
    document.documentElement.dataset.density = preferences.density;
  }, [preferences]);
  return preferences;
}
