import {
  createContext,
  useCallback,
  useContext,
  useState,
  useSyncExternalStore,
  type Dispatch,
  type ReactNode,
  type SetStateAction,
} from "react";

// One store per workbench window. Never writes drafts or credentials to disk.
export function createWorkspaceDrafts() {
  const values = new Map<string, unknown>();
  const listeners = new Set<() => void>();
  return {
    read: (key: string, fallback: unknown) => (values.has(key) ? values.get(key) : fallback),
    write(key: string, value: unknown) {
      values.set(key, value);
      listeners.forEach(listener => listener());
    },
    subscribe(listener: () => void) {
      listeners.add(listener);
      return () => {
        listeners.delete(listener);
      };
    },
  };
}
const DraftContext = createContext<{
  store: ReturnType<typeof createWorkspaceDrafts>;
  namespace: string;
} | null>(null);

export function WorkspaceDraftScope({
  store,
  namespace,
  children,
}: {
  store: ReturnType<typeof createWorkspaceDrafts>;
  namespace: string;
  children: ReactNode;
}) {
  return <DraftContext.Provider value={{ store, namespace }}>{children}</DraftContext.Provider>;
}

/** Retains explicitly selected plugin input across navigation, until this window reloads. */
export function useWorkspaceDraft<T>(
  slot: string,
  initial: T | (() => T),
): [T, Dispatch<SetStateAction<T>>] {
  const context = useContext(DraftContext);
  if (!context) throw new Error("Workspace drafts require a workspace scope");
  const { store, namespace } = context;
  const key = JSON.stringify([namespace, slot]);
  const [fallback] = useState(initial);
  const read = useCallback(() => store.read(key, fallback) as T, [store, key, fallback]);
  const value = useSyncExternalStore(store.subscribe, read);
  const setValue = useCallback<Dispatch<SetStateAction<T>>>(
    next => {
      store.write(key, typeof next === "function" ? (next as (previous: T) => T)(read()) : next);
    },
    [store, key, read],
  );
  return [value, setValue];
}

/** An uncertain request retains its ID only while its input and destination match. */
export function useWorkspaceRequestId(slot: string, input: string) {
  const [pending, setPending] = useWorkspaceDraft<{ input: string; id: string } | null>(slot, null);
  return [
    pending?.input === input ? pending.id : null,
    (id: string | null) => setPending(id === null ? null : { input, id }),
  ] as const;
}
