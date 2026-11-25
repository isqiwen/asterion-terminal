import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { tradingRequest, type TradingCall } from "../bridge/trading";
import { strategyRequest, type StrategyCall } from "../bridge/strategy";
import type { TradingConnection } from "../bridge/client";
import type { State as EmergencyState } from "../generated/asterion/emergency_pb";
import {
  BrokerState,
  InstanceEventKind,
  InstanceState,
  type Account,
  type DayClose,
  type Health,
  type InstanceStatus,
  type Order,
  type Position,
  type Progress,
  type Response,
  type Session,
  type Strategies,
  type Trade,
} from "../generated/asterion/trading_pb";
import { translate } from "../i18n";
import {
  BackendError,
  asDisplayError,
  isDeselectedService,
  type DisplayError,
} from "../i18n/errors";
import { useActionError } from "./action-error";
import { notify } from "./notifications";
import { useIntervalPoll, useSequentialPoll } from "./polling";

// What Terminal last read from one account's Trading service.
export type TradingView = {
  instance?: string;
  account?: Account;
  // The account's session: absent until read, and until the account is opened.
  live?: Session;
  positions?: { rows: readonly Position[]; readAt: Date };
  initialized?: boolean;
  health?: Health;
  strategies?: Strategies;
  // Available when routine trading protocol differs, so braking stays live.
  emergency?: EmergencyState;
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
  if (!status || !instance) return { instance: "", eventMs: -1n, failed: false };
  const same = before?.instance === instance;
  const since = !before ? (status.events.at(-1)?.timeMs ?? 0n) : same ? before.eventMs : -1n;
  const bodies = status.events
    .filter(event => event.timeMs > since && event.kind === InstanceEventKind.BATCH_ABANDONED)
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

const running = new Set([InstanceState.STARTING, InstanceState.WARMING, InstanceState.READY]);

// At startup, switch to trading as soon as an account may send orders or runs a
// strategy. Cancel once the user navigates or after a short startup window so an
// offline account never hijacks the workspace later.
export function OpenLiveTrading({
  accounts,
  cancelled = false,
  open,
}: {
  accounts: number;
  cancelled?: boolean;
  open: () => void;
}) {
  const { views } = useTradingAccounts();
  const decided = useRef(false);
  useEffect(() => {
    if (cancelled) decided.current = true;
  }, [cancelled]);
  useEffect(() => {
    if (decided.current || !accounts) return;
    const timer = window.setTimeout(() => {
      decided.current = true;
    }, 3000);
    return () => window.clearTimeout(timer);
  }, [accounts]);
  useEffect(() => {
    const read = Object.values(views);
    if (decided.current || !accounts || read.length === 0) return;
    const active = read.some(
      view =>
        view.live?.authorizedMs !== undefined ||
        running.has(view.strategies?.instance?.state ?? InstanceState.UNSPECIFIED),
    );
    if (active) {
      decided.current = true;
      open();
    } else if (read.length >= accounts) {
      decided.current = true;
    }
  }, [views, accounts, open]);
  return null;
}

function merge(view: TradingView, response: Response, readAt: Date): TradingView {
  const result = response.result;
  if (result.case === "getEmergency" || result.case === "emergencyControl")
    return { ...view, emergency: result.value };
  const next = { ...view, failed: undefined, emergency: undefined };
  if (result.case === "getHealth") return { ...next, health: result.value };
  if (result.value?.$typeName === "trading.Strategies")
    return { ...next, strategies: result.value };
  if (result.value?.$typeName !== "trading.Account") return next;
  const account = result.value;
  const live = account.session;
  const positions =
    live?.brokerState === BrokerState.READY && live.positionsReconciled && !live.recoveryRequired
      ? { rows: live.positions, readAt }
      : live && view.positions;
  return { ...next, account, live, positions, initialized: !!live };
}

function isProtocolMismatch(reason: unknown) {
  return (
    reason instanceof BackendError &&
    (reason.diagnostic === "trading protocol differs from this Terminal" ||
      reason.diagnostic === "unknown Protobuf field")
  );
}

// Every registered account is read once a second with one read in flight, and
// its health and strategy instance every five seconds, hidden window or not.
export function TradingAccountsProvider({
  connections,
  children,
}: {
  connections: readonly (Pick<TradingConnection, "id" | "revision"> & { instance?: string })[];
  children: ReactNode;
}) {
  const key = JSON.stringify(connections.map(({ id, revision }) => [id, revision]));
  const identities = useMemo(
    () =>
      new Map<string, string>(
        JSON.parse(key).map(([id, revision]: string[]) => [id, JSON.stringify([id, revision])]),
      ),
    [key],
  );
  const [views, setViews] = useState<Record<string, TradingView>>({});
  const alerted = useRef<Record<string, Alerted | undefined>>({});
  const revisions = useRef<Record<string, number>>({});
  const accept = useCallback(
    (identity: string, responses: Response[], startedAt: number, readAt: Date) => {
      const currentRevision = revisions.current[identity] ?? 0;
      if (startedAt !== currentRevision) return;
      revisions.current[identity] = currentRevision + 1;
      setViews(current => ({
        ...current,
        [identity]: responses.reduce(
          (view, reply) => merge(view, reply, readAt),
          current[identity] ?? {},
        ),
      }));
    },
    [],
  );
  const send = useCallback(
    async (id: string, operation: TradingCall) => {
      const identity = identities.get(id)!;
      const startedAt = revisions.current[identity] ?? 0;
      const response = await tradingRequest(id, operation);
      if (
        response.result.value?.$typeName === "trading.Account" ||
        response.result.value?.$typeName === "trading.Strategies" ||
        response.result.case === "emergencyControl" ||
        response.result.case === "getEmergency"
      ) {
        accept(identity, [response], startedAt, new Date());
      }
      return response;
    },
    [accept, identities],
  );
  useEffect(() => {
    if (!identities.size) return;
    const reading = new Set<string>();
    // Accounts whose last read failed or whose work stalled: each is reported
    // once, when it begins.
    const failing = new Set<string>();
    const stalling = new Set<string>();
    const breached = new Map<string, Set<string>>();
    const mismatched = new Set<string>();
    let ticks = 0;
    let stopped = false;
    const read = async (id: string, health: boolean) => {
      if (reading.has(id)) return;
      reading.add(id);
      const identity = identities.get(id)!;
      const startedAt = revisions.current[identity] ?? 0;
      try {
        const responses: Response[] = [];
        if (mismatched.has(id)) {
          responses.push(await tradingRequest(id, { case: "getHealth", value: {} }));
          mismatched.delete(id);
        }
        const accountReply = await tradingRequest(id, { case: "getAccount", value: {} });
        const accountReadAt = new Date();
        responses.push(accountReply);
        if (!stopped && accountReply.result.case === "getAccount") {
          const previous = breached.get(id);
          const next = new Set(accountReply.result.value.session?.riskBreaches ?? []);
          for (const code of next)
            if (!previous?.has(code))
              notify(new BackendError("risk_breached", `${id}: ${code}`), {
                namespace: "asterion.terminal.trading",
              });
          breached.set(id, next);
        }
        if (health) {
          const beat =
            responses[0]?.result.case === "getHealth"
              ? responses[0]
              : await tradingRequest(id, { case: "getHealth", value: {} });
          if (responses[0] !== beat) responses.push(beat);
          if (beat.result.case !== "getHealth" || !stalled(beat.result.value)) stalling.delete(id);
          else if (!stalling.has(id)) {
            stalling.add(id);
            notify(new BackendError("trading_stalled", id), {
              namespace: "asterion.terminal.trading",
            });
          }
          const strategies = await tradingRequest(id, { case: "getStrategies", value: {} });
          if (strategies.result.case === "getStrategies") responses.push(strategies);
          if (!stopped && strategies.result.case === "getStrategies")
            alerted.current[identity] = alert(
              id,
              strategies.result.value.instance,
              alerted.current[identity],
            );
        }
        if (!stopped) accept(identity, responses, startedAt, accountReadAt);
        failing.delete(id);
      } catch (reason) {
        if (stopped || isDeselectedService(reason)) return;
        if (!failing.has(id)) {
          failing.add(id);
          notify(reason, { namespace: "asterion.terminal.trading" });
        }
        let emergency: EmergencyState | undefined;
        if (isProtocolMismatch(reason)) {
          mismatched.add(id);
          try {
            const fallback = await tradingRequest(id, { case: "getEmergency", value: {} });
            if (fallback.result.case === "getEmergency") emergency = fallback.result.value;
          } catch {
            emergency = undefined;
          }
        }
        if (!stopped)
          setViews(current => ({
            ...current,
            [identity]: {
              ...current[identity],
              emergency: emergency ?? current[identity]?.emergency,
              failed: asDisplayError(reason),
            },
          }));
      } finally {
        reading.delete(id);
      }
    };
    const tick = () => {
      const health = ticks++ % 5 === 0;
      for (const id of identities.keys()) void read(id, health);
    };
    tick();
    const timer = window.setInterval(tick, 1000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [identities, accept]);
  const value = useMemo(() => {
    const instances = new Map(connections.map(item => [item.id, item.instance]));
    return {
      views: Object.fromEntries(
        [...identities]
          .filter(([, identity]) => views[identity])
          .map(([id, identity]) => {
            const instance = instances.get(id);
            return [id, instance ? { ...views[identity]!, instance } : views[identity]!];
          }),
      ),
      send,
    };
  }, [connections, identities, views, send]);
  return <Accounts.Provider value={value}>{children}</Accounts.Provider>;
}

// One account's strategy host, polled every 3 s while mounted: its packages,
// running instance, and mutation controls.
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
    return () => {
      active.current = false;
    };
  }, [refresh]);
  useIntervalPoll(() => void refresh(), 3000, [refresh]);

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

export type RecordedDay = {
  connection: string;
  day: string;
  liveDay: string;
  offset: number;
  total: number;
  days: string[];
  trades: Trade[];
  close?: DayClose;
};

// Polls one trading day's fills (continuously for today, once for historical days).
export function useTradingDayFills(
  connection: string,
  day: string,
  liveDay: string,
  offset: number,
  limit: number,
  open: boolean,
) {
  const [loading, setLoading] = useState(false);
  const [recorded, setRecorded] = useState<RecordedDay>({
    connection: "",
    day: "",
    liveDay: "",
    offset: 0,
    total: 0,
    days: [],
    trades: [],
  });
  const firstTick = useRef(true);
  useEffect(() => {
    firstTick.current = true;
  }, [connection, day, offset, liveDay, limit, open]);
  useSequentialPoll(
    async isActive => {
      if (firstTick.current) {
        firstTick.current = false;
        setLoading(true);
      }
      try {
        const reply = await tradingRequest(connection, {
          case: "getDay",
          value: { tradingDay: day, offset, limit },
        });
        if (isActive() && reply.result.case === "getDay")
          setRecorded({
            connection,
            day,
            liveDay,
            offset,
            total: reply.result.value.total,
            days: reply.result.value.tradingDays,
            trades: reply.result.value.trades,
            close: reply.result.value.close,
          });
      } catch (failure) {
        if (isActive()) notify(failure, { namespace: "asterion.terminal.trading" });
      } finally {
        if (isActive()) setLoading(false);
      }
      return !day;
    },
    1000,
    open,
    [connection, day, offset, liveDay, limit],
  );
  return { loading, recorded };
}

export type RecordedOrders = {
  connection: string;
  day: string;
  offset: number;
  rows: Order[];
};

// Polls an account's paged orders every 1 s while the full orders tab is open.
export function useTradingOrders(
  connection: string,
  tradingDay: string,
  offset: number,
  limit: number,
  open: boolean,
) {
  const [loading, setLoading] = useState(false);
  const [orders, setOrders] = useState<RecordedOrders>();
  const firstTick = useRef(true);
  useEffect(() => {
    firstTick.current = true;
  }, [connection, offset, tradingDay, limit, open]);
  useSequentialPoll(
    async isActive => {
      if (firstTick.current) {
        firstTick.current = false;
        setLoading(true);
      }
      try {
        const reply = await tradingRequest(connection, {
          case: "getOrders",
          value: { offset, limit },
        });
        if (isActive() && reply.result.case === "getOrders")
          setOrders({
            connection,
            day: tradingDay,
            offset,
            rows: reply.result.value.orders,
          });
      } catch (failure) {
        if (isActive()) notify(failure, { namespace: "asterion.terminal.trading" });
      } finally {
        if (isActive()) setLoading(false);
      }
      return true;
    },
    1000,
    open,
    [connection, offset, tradingDay, limit],
  );
  return { loading, orders };
}
