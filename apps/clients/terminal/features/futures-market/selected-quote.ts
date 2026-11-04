import { useSyncExternalStore } from "react";
// The contract last chosen in either market view of this window.
let selected = "";
const listeners = new Set<() => void>();
export function selectQuote(id: string) {
  if (selected === id) return;
  selected = id;
  listeners.forEach(listener => listener());
}
const subscribe = (listener: () => void) => {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
};
export function useSelectedQuote() {
  return useSyncExternalStore(subscribe, () => selected);
}
