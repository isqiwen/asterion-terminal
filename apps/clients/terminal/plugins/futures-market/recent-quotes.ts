import { useSyncExternalStore } from "react";
// Read-only browsing history shared by the three market views in this window.
// No credentials, provider configuration or trading state are persisted here.
let recent: readonly string[] = [];
const listeners = new Set<() => void>();
export function recordQuoteVisit(id: string) {
  if (recent[0] === id) return;
  recent = [id, ...recent.filter(value => value !== id)].slice(0, 50);
  listeners.forEach(listener => listener());
}
const subscribe = (listener: () => void) => {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
};
export function useRecentQuotes() {
  return useSyncExternalStore(subscribe, () => recent);
}
