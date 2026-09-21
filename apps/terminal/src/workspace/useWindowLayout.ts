import { useEffect, useRef, useState, type SetStateAction } from "react";
import { invoke } from "@tauri-apps/api/core";
import { nativeDesktop } from "../deployment/desktop";
export type LayoutContract<T> = {
  parse: (raw: string | null) => T;
  editable: readonly (keyof T)[];
  initial: (value: T) => T;
};
export function useWindowLayout<T extends object>(contract: LayoutContract<T>) {
  type Snapshot = { id: string; revision: number; layout: T };
  const { parse: parseLayout, editable } = contract;
  const [initial] = useState(() => {
    try {
      const value = parseLayout(
        nativeDesktop ? null : localStorage.getItem("asterion.layout"),
      );
      return { layout: contract.initial(value), error: "" };
    } catch (error) {
      return { layout: parseLayout(null), error: String(error) };
    }
  });
  const [layout, display] = useState(initial.layout);
  const [ready, setReady] = useState(!nativeDesktop && !initial.error);
  const [error, setError] = useState(initial.error);
  const current = useRef(layout);
  const queue = useRef(Promise.resolve());
  const pending = useRef(0);
  const alive = useRef(true);
  const failure = useRef("");
  function accept(snapshot: Snapshot) {
    if (!alive.current) return;
    const next = parseLayout(JSON.stringify(snapshot.layout));
    current.current = next;
    display((old) =>
      JSON.stringify(old) === JSON.stringify(next) ? old : next,
    );
    setReady(true);
  }
  useEffect(() => {
    alive.current = true;
    if (!nativeDesktop)
      return () => {
        alive.current = false;
      };
    async function refresh() {
      if (pending.current) return;
      try {
        const result = await invoke<Snapshot>("desktop_workspace_read");
        if (!pending.current) accept(result);
      } catch (e) {
        if (alive.current) setError(String(e));
      }
    }
    void refresh();
    const timer = setInterval(() => void refresh(), 500);
    return () => {
      alive.current = false;
      clearInterval(timer);
    };
  }, []);
  function setLayout(action: SetStateAction<T>) {
    if (!ready) return;
    const before = current.current;
    const next = typeof action === "function" ? action(before) : action;
    const patch = Object.fromEntries(
      editable.filter((k) => before[k] !== next[k]).map((k) => [k, next[k]]),
    );
    if (!Object.keys(patch).length) return;
    current.current = next;
    display(next);
    if (!nativeDesktop) {
      localStorage.setItem("asterion.layout", JSON.stringify(next));
      return;
    }
    pending.current++;
    queue.current = queue.current.then(async () => {
      try {
        for (let attempt = 0; attempt < 5; attempt++) {
          const remote = await invoke<Snapshot>("desktop_workspace_read");
          try {
            const result = await invoke<Snapshot>("desktop_workspace_patch", {
              expected: remote.revision,
              patch,
            });
            if (pending.current === 1) accept(result);
            failure.current = "";
            setError("");
            return;
          } catch (e) {
            if (!String(e).includes("WORKSPACE_CONFLICT") || attempt === 4)
              throw e;
          }
        }
      } catch (e) {
        failure.current = String(e);
        if (alive.current) setError(String(e));
      } finally {
        pending.current--;
      }
    });
  }
  async function flush() {
    await queue.current;
    if (failure.current) throw new Error(failure.current);
  }
  return { layout, setLayout, ready, error, flush };
}
