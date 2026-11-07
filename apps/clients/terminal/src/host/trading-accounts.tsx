import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { tradingRequest, type TradingCall } from "../bridge/trading";
import type {
  Account,
  Health,
  Progress,
  Response,
  Session,
} from "../generated/asterion/trading_pb";
import { BackendError, asDisplayError, type DisplayError } from "../i18n/errors";
import { notify } from "./notifications";

// What Terminal last read from one account's Trading service.
export type TradingView = {
  account?: Account;
  // The account's session: absent until read, and until the account is opened.
  live?: Session;
  initialized?: boolean;
  health?: Health;
  // The last read failed; the fields above are what was read before.
  failed?: DisplayError;
};
export type TradingAccounts = {
  views: Readonly<Record<string, TradingView>>;
  send: (account: string, operation: TradingCall) => Promise<Response>;
};

// Some part of the Trading service's work has not advanced for 30 seconds.
export function stalled(health: Health | undefined) {
  const late = (p: Progress | undefined, periodic: boolean) =>
    !!p?.observed && Number(p.idleMs) > 30000 && (periodic || p.pending);
  return (
    !!health &&
    (late(health.io, true) ||
      late(health.state, true) ||
      late(health.persistence, false) ||
      late(health.initialization, false) ||
      late(health.command, false))
  );
}

const Accounts = createContext<TradingAccounts>({ views: {}, send: tradingRequest });
export const useTradingAccounts = () => useContext(Accounts);

function merge(view: TradingView, response: Response): TradingView {
  const next = { ...view, failed: undefined };
  const result = response.result;
  if (result.case === "getHealth") return { ...next, health: result.value };
  if (result.value?.$typeName !== "trading.Account") return next;
  const account = result.value;
  return { ...next, account, live: account.session, initialized: !!account.session };
}

// Every registered account is read once a second with one read in flight, and
// its health every five seconds. Hidden windows stop reading.
export function TradingAccountsProvider({
  ids,
  active,
  protocol,
  children,
}: {
  ids: readonly string[];
  active: boolean;
  // The Trading protocol this Terminal was built with.
  protocol: string | undefined;
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
    // Only a notice: refusing the connection would also take away cancelling and closing.
    const outdated = new Set<string>();
    let ticks = 0;
    let stopped = false;
    const read = async (id: string, health: boolean) => {
      if (reading.has(id)) return;
      reading.add(id);
      const command = commands.current[id];
      try {
        const responses = [await tradingRequest(id, { case: "getAccount", value: {} })];
        if (health) {
          const beat = await tradingRequest(id, { case: "getHealth", value: {} });
          responses.push(beat);
          if (beat.result.case === "getHealth" && beat.result.value.protocol !== protocol) {
            if (!outdated.has(id)) notify(new BackendError("trading_protocol", id));
            outdated.add(id);
          } else outdated.delete(id);
          if (beat.result.case !== "getHealth" || !stalled(beat.result.value)) stalling.delete(id);
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
  }, [key, active, protocol, accept]);
  const send = useCallback<TradingAccounts["send"]>(
    async (id, operation) => {
      const next = () => (commands.current[id] = (commands.current[id] ?? 0) + 1);
      next();
      try {
        const response = await tradingRequest(id, operation);
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
