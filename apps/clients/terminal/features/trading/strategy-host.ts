import { useCallback, useEffect, useRef, useState } from "react";
import type { Strategies } from "../../src/generated/asterion/trading_pb";
import { strategyRequest, type StrategyCall } from "../../src/bridge/strategy";
import { useActionError } from "../../src/host/action-error";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";

// One account's strategy host, read every 3 s: its packages and instance, and
// the commands that change them. The trading page shows the instance beside
// the account and the packages below it, from this one state.
export function useStrategyHost(account: string) {
  const [state, setState] = useState<Strategies>();
  const [busy, setBusy] = useState(false);
  const [stopping, setStopping] = useState(false);
  const [reading, setReading] = useState(false);
  const [fresh, setFresh] = useState(false);
  const [observedAt, setObservedAt] = useState<Date>();
  const [readError, setReadError] = useState<DisplayError>("");
  const sequence = useRef(0);
  const active = useRef(true);
  const readPending = useRef(false);
  const commandPending = useRef(false);
  const stopPending = useRef(false);
  const preparing = useRef(false);
  const accept = (next: Strategies) => {
    setState(next);
    setFresh(true);
    setReadError("");
    setObservedAt(new Date());
  };
  const refresh = useCallback(async () => {
    if (readPending.current || commandPending.current || stopPending.current) return;
    readPending.current = true;
    setReading(true);
    const current = ++sequence.current;
    try {
      const next = await strategyRequest(account, { case: "getStrategies", value: {} });
      if (active.current && current === sequence.current) accept(next);
    } catch (reason) {
      if (active.current && current === sequence.current) {
        setFresh(false);
        setReadError(asDisplayError(reason));
      }
    } finally {
      readPending.current = false;
      if (active.current) setReading(false);
    }
  }, [account]);
  useEffect(() => {
    active.current = true;
    ++sequence.current;
    void refresh();
    const timer = window.setInterval(() => void refresh(), 3000);
    return () => {
      active.current = false;
      window.clearInterval(timer);
    };
  }, [refresh]);

  const { error, attempt } = useActionError();
  async function mutate(operation: StrategyCall) {
    const stop = operation.case === "stopStrategy";
    const pending = stop ? stopPending : commandPending;
    if (pending.current || (!stop && (preparing.current || stopPending.current || !fresh)))
      return false;
    pending.current = true;
    (stop ? setStopping : setBusy)(true);
    const current = ++sequence.current;
    const done = await attempt(async () => {
      const next = await strategyRequest(account, operation);
      if (active.current && current === sequence.current) accept(next);
    });
    if (!done && active.current && current === sequence.current) setFresh(false);
    pending.current = false;
    if (active.current) (stop ? setStopping : setBusy)(false);
    return done;
  }
  async function perform(action: () => Promise<void>) {
    if (preparing.current || commandPending.current) return;
    preparing.current = true;
    setBusy(true);
    await attempt(action);
    preparing.current = false;
    setBusy(false);
  }
  return {
    state,
    busy,
    stopping,
    reading,
    fresh,
    observedAt,
    readError,
    error,
    refresh,
    mutate,
    perform,
  };
}
export type StrategyHost = ReturnType<typeof useStrategyHost>;
