import { useEffect, useRef } from "react";

// Runs `read` on a fixed interval while `delayMs` is non-null; restarts when `deps` change.
export function useIntervalPoll(
  read: () => void,
  delayMs: number | null,
  deps: readonly unknown[] = [],
) {
  const latest = useRef(read);
  latest.current = read;
  useEffect(() => {
    if (delayMs === null) return;
    const timer = window.setInterval(() => latest.current(), delayMs);
    return () => window.clearInterval(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [delayMs, ...deps]);
}

// Runs `step` immediately when `enabled`, then schedules the next call `delayMs` after
// completion while `step` resolves to true. Changing `deps` cancels pending ticks.
export function useSequentialPoll(
  step: (isActive: () => boolean) => Promise<boolean>,
  delayMs: number,
  enabled: boolean,
  deps: readonly unknown[],
) {
  const latest = useRef(step);
  latest.current = step;
  useEffect(() => {
    if (!enabled) return;
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
  }, [enabled, delayMs, ...deps]);
}
