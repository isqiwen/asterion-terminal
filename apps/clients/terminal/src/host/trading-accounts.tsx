import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { tradingRequest, type TradingFields, type TradingOperation } from "../bridge/trading";
import type {
  Health,
  LiveSnapshot,
  Response,
  TradingAccount,
} from "../generated/asterion/trading_pb";
import type { Progress } from "../generated/asterion/health_pb";
import { BackendError, asDisplayError, type DisplayError } from "../i18n/errors";
import { notify } from "./notifications";

// What Terminal last read from one account's Trading service.
export type TradingView = {
  account?: TradingAccount;
  // Absent until read, and while the service has no account policy yet.
  live?: LiveSnapshot;
  initialized?: boolean;
  health?: Health;
  // The last read failed; the fields above are what was read before.
  failed?: DisplayError;
};
export type TradingAccounts = {
  views: Readonly<Record<string, TradingView>>;
  send: (account: string, operation: TradingOperation, fields?: TradingFields) => Promise<Response>;
};

// Some part of the Trading service's work has not advanced for 30 seconds.
export function stalled(health: Health | undefined) {
  const e = health?.execution;
  const late = (p: Progress | undefined, periodic: boolean) =>
    !!p?.observed && Number(p.ageMs) > 30000 && (periodic || p.pending);
  return (
    !!e &&
    (late(e.io, true) ||
      late(e.state, true) ||
      late(e.persistence, false) ||
      late(e.initialization, false) ||
      late(e.command, false))
  );
}

const Accounts = createContext<TradingAccounts>({ views: {}, send: tradingRequest });
export const useTradingAccounts = () => useContext(Accounts);

function merge(view: TradingView, response: Response): TradingView {
  const next = { ...view, account: response.account, failed: undefined };
  switch (response.result.case) {
    case "live":
      return { ...next, live: response.result.value, initialized: true };
    case "uninitialized":
      return { ...next, live: undefined, initialized: false };
    case "health":
      return { ...next, health: response.result.value };
  }
  return next;
}

// Every registered account is read once a second with one read in flight, and
// its health every five seconds. Hidden windows stop reading.
export function TradingAccountsProvider({
  ids,
  active,
  children,
}: {
  ids: readonly string[];
  active: boolean;
  children: ReactNode;
}) {
  const [views, setViews] = useState<Record<string, TradingView>>({});
  // A read that began before a command was answered may show the account as
  // it was before that command, so it is dropped.
  const commands = useRef<Record<string, number>>({});
  const accept = useCallback((id: string, responses: Response[]) => {
    setViews(current => ({ ...current, [id]: responses.reduce(merge, current[id] ?? {}) }));
  }, []);
  const key = ids.join("\n");
  useEffect(() => {
    if (!active || !key) return;
    const reading = new Set<string>();
    // Accounts whose last read failed or whose work stalled: each is reported
    // once, when it begins.
    const failing = new Set<string>();
    const stalling = new Set<string>();
    let ticks = 0;
    let stopped = false;
    const read = async (id: string, health: boolean) => {
      if (reading.has(id)) return;
      reading.add(id);
      const command = commands.current[id];
      try {
        const responses = [await tradingRequest(id, { case: "attach", value: {} })];
        if (health) {
          const beat = await tradingRequest(id, { case: "heartbeat", value: {} });
          responses.push(beat);
          if (beat.result.case !== "health" || !stalled(beat.result.value)) stalling.delete(id);
          else if (!stalling.has(id)) {
            stalling.add(id);
            notify(new BackendError("trading_stalled", id));
          }
        }
        if (!stopped && command === commands.current[id]) accept(id, responses);
        failing.delete(id);
      } catch (reason) {
        if (!failing.has(id)) {
          failing.add(id);
          notify(reason, { namespace: "asterion.terminal.trading" });
        }
        if (!stopped)
          setViews(current => ({
            ...current,
            [id]: { ...current[id], failed: asDisplayError(reason) },
          }));
      } finally {
        reading.delete(id);
      }
    };
    const tick = () => {
      const health = ticks++ % 5 === 0;
      for (const id of key.split("\n")) void read(id, health);
    };
    tick();
    const timer = window.setInterval(tick, 1000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [key, active, accept]);
  const send = useCallback<TradingAccounts["send"]>(
    async (id, operation, fields) => {
      const next = () => (commands.current[id] = (commands.current[id] ?? 0) + 1);
      next();
      try {
        const response = await tradingRequest(id, operation, fields);
        accept(id, [response]);
        return response;
      } finally {
        next();
      }
    },
    [accept],
  );
  const value = {
    views: Object.fromEntries(ids.filter(id => views[id]).map(id => [id, views[id]!])),
    send,
  };
  return <Accounts.Provider value={value}>{children}</Accounts.Provider>;
}
