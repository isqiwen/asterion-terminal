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
import {
  InstanceState,
  type Account,
  type Health,
  type InstanceStatus,
  type Progress,
  type Response,
  type Session,
} from "../generated/asterion/trading_pb";
import { translate } from "../i18n";
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

// How far an account's instance alerts have gone: the instance, its latest
// event seen and whether its failure was told.
type Alerted = { instance: string; eventMs: bigint; failed: boolean };

// An instance that fails or drops a batch: the notification centre and a
// desktop notification, seen with the window hidden. Events from before
// Terminal first read the account are history; a failure is told once.
function alert(account: string, status: InstanceStatus | undefined, before: Alerted | undefined) {
  const instance = status?.instance?.id;
  if (!status || !instance) return before;
  const same = before?.instance === instance;
  const since = !before ? (status.events.at(-1)?.timeMs ?? 0n) : same ? before.eventMs : -1n;
  const bodies = status.events
    .filter(event => event.timeMs > since && event.text.startsWith("batch abandoned"))
    .map(event => event.text);
  const failed = status.state === InstanceState.FAILED;
  if (failed && !(same && before?.failed)) bodies.push(status.error);
  for (const body of bodies) {
    notify(new BackendError("strategy_instance", `${account}/${instance}: ${body}`), {
      namespace: "asterion.terminal.trading",
    });
    if (typeof Notification !== "undefined")
      new Notification(translate("host", "策略实例 {id}", { id: instance }), { body });
  }
  return { instance, eventMs: status.events.at(-1)?.timeMs ?? since, failed };
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
// its health and strategy instance every five seconds, hidden window or not.
export function TradingAccountsProvider({
  ids,
  protocol,
  children,
}: {
  ids: readonly string[];
  // The Trading protocol this Terminal was built with.
  protocol: string | undefined;
  children: ReactNode;
}) {
  const [views, setViews] = useState<Record<string, TradingView>>({});
  const alerted = useRef<Record<string, Alerted | undefined>>({});
  const accept = useCallback((id: string, responses: Response[]) => {
    setViews(current => ({ ...current, [id]: responses.reduce(merge, current[id] ?? {}) }));
  }, []);
  const key = ids.join("\n");
  useEffect(() => {
    if (!key) return;
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
          const strategies = await tradingRequest(id, { case: "getStrategies", value: {} });
          if (strategies.result.case === "getStrategies")
            alerted.current[id] = alert(id, strategies.result.value.instance, alerted.current[id]);
        }
        if (!stopped) accept(id, responses);
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
  }, [key, protocol, accept]);
  const value = {
    views: Object.fromEntries(ids.filter(id => views[id]).map(id => [id, views[id]!])),
    send: tradingRequest,
  };
  return <Accounts.Provider value={value}>{children}</Accounts.Provider>;
}
