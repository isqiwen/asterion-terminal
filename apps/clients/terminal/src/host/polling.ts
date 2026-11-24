import { createContext, useContext, useEffect, useRef } from "react";

// False while a kept-alive workspace view is hidden behind another tab.
export const WorkspaceActiveContext = createContext(true);
export const useWorkspaceActive = () => useContext(WorkspaceActiveContext);

// Runs `read` on a fixed interval while `delayMs` is non-null and the view is visible.
export function useIntervalPoll(
  read: () => void,
  delayMs: number | null,
  deps: readonly unknown[] = [],
) {
  const workspaceActive = useWorkspaceActive();
  const activeDelay = workspaceActive ? delayMs : null;
  const latest = useRef(read);
  latest.current = read;
  useEffect(() => {
    if (activeDelay === null) return;
    const timer = window.setInterval(() => latest.current(), activeDelay);
    return () => window.clearInterval(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeDelay, ...deps]);
}

// Runs `step` immediately when `enabled` and visible, then schedules the next call `delayMs` after
// completion while `step` resolves to true. Changing `deps` cancels pending ticks.
export function useSequentialPoll(
  step: (isActive: () => boolean) => Promise<boolean>,
  delayMs: number,
  enabled: boolean,
  deps: readonly unknown[],
) {
  const workspaceActive = useWorkspaceActive();
  const activeEnabled = enabled && workspaceActive;
  const latest = useRef(step);
  latest.current = step;
  useEffect(() => {
    if (!activeEnabled) return;
    let active = true;
    let timer: number | undefined;
    const run = async () => {
      const again = await latest.current(() => active);
      if (active && again) timer = window.setTimeout(() => void run(), delayMs);
    };
    void run();
    return () => {
      active = false;
      window.clearTimeout(timer);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeEnabled, delayMs, ...deps]);
}
