import { useEffect, useRef, useState, type SetStateAction } from "react";
import { invoke } from "@tauri-apps/api/core";
import { nativeDesktop } from "../deployment/desktop";
import { parseLayout, views, type Layout, type View } from "./layout";
type Snapshot = { id: string; revision: number; layout: Layout };
const editable = [
  "version",
  "viewport",
  "dock",
  "view",
  "inspector",
  "tasks",
  "taskHeight",
  "marketRatio",
  "snapshot",
  "contract",
  "section",
  "linkGroup",
  "locked",
] as const;
export function useWindowLayout() {
  const [layout, display] = useState(() =>
    (() => {
      const value = parseLayout(localStorage.getItem("asterion.layout"));
      const view = new URLSearchParams(location.search).get("view") as View;
      return !nativeDesktop && views.includes(view)
        ? { ...value, view }
        : value;
    })(),
  );
  const [ready, setReady] = useState(!nativeDesktop);
  const [error, setError] = useState("");
  const current = useRef(layout);
  const queue = useRef(Promise.resolve());
  const pending = useRef(0);
  const alive = useRef(true);
  const failure = useRef("");
  const migrated = useRef(false);
  function accept(snapshot: Snapshot) {
    if (!alive.current) return;
    const next = parseLayout(JSON.stringify(snapshot.layout));
    current.current = next;
    display((old) =>
      JSON.stringify(old) === JSON.stringify(next) ? old : next,
    );
    // Non-sensitive mirror supports older layouts and browser development.
    localStorage.setItem(
      snapshot.id === "main"
        ? "asterion.layout"
        : `asterion.layout.${snapshot.id}`,
      JSON.stringify(next),
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
        let result = await invoke<Snapshot>("desktop_workspace_read");
        if (
          !migrated.current &&
          result.id === "main" &&
          result.revision === 0 &&
          localStorage.getItem("asterion.layout")
        ) {
          migrated.current = true;
          const legacy = parseLayout(localStorage.getItem("asterion.layout"));
          result = await invoke<Snapshot>("desktop_workspace_patch", {
            expected: 0,
            patch: Object.fromEntries(editable.map((k) => [k, legacy[k]])),
          });
        }
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
  function setLayout(action: SetStateAction<Layout>) {
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
