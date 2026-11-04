import { useCallback, useEffect, useRef, useState } from "react";
import type { InstanceStatus, Package, State } from "../../src/generated/asterion/strategy_pb";
import { lab } from "../../src/bridge/lab";
import { strategyRequest } from "../../src/bridge/strategy";
import { BackendError, ErrorNotice, asDisplayError, type DisplayError, notify } from "../contract";
import { t } from "./live-shared";
import { StrategyInstance, type StrategyAccount } from "./StrategyInstance";
import { StrategyMonitor } from "./StrategyMonitor";
import "./strategy.css";

type Props = { account: string; repository: string; live?: StrategyAccount; connected?: boolean };
type Operation = NonNullable<Parameters<typeof strategyRequest>[1]>;

// An instance that fails or drops a batch while Terminal is open: a desktop
// notification, seen with the window hidden, and the notification centre.
function notifyDesktop(previous: InstanceStatus | undefined, next: InstanceStatus | undefined) {
  const id = next?.configuration?.id;
  if (!id || previous?.configuration?.id !== id) return;
  const seen = previous.events.at(-1)?.timeMs ?? 0n;
  const alerts = next.events
    .filter(event => event.timeMs > seen && event.text.startsWith("batch abandoned"))
    .map(event => event.text);
  if (next.state === "failed" && previous.state !== "failed") alerts.push(next.error);
  for (const body of alerts) {
    notify(new BackendError("strategy_instance", `${id}: ${body}`), {
      namespace: "asterion.terminal.trading",
    });
    if (typeof Notification !== "undefined") new Notification(t("策略实例 {id}", { id }), { body });
  }
}

export function StrategyPackages(props: Props) {
  return <AccountStrategies key={props.account} {...props} />;
}

function AccountStrategies({ account, repository, live, connected = false }: Props) {
  const [strategies, setStrategies] = useState<string[]>([]);
  const [strategy, setStrategy] = useState("");
  const [preparation, setPreparation] = useState<{ repository: string; value: Package }>();
  const prepared = preparation?.repository === repository ? preparation.value : undefined;
  const [state, setState] = useState<State>();
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
  const seen = useRef<InstanceStatus | undefined>(undefined);
  const accept = (next: State) => {
    notifyDesktop(seen.current, next.instance);
    seen.current = next.instance;
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
      const next = await strategyRequest(account, { case: "inspect", value: {} });
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
    if (!repository) return;
    let current = true;
    void lab({ case: "strategies", value: { repository } }, "strategies").then(
      listed => current && setStrategies(listed.strategies.map(item => item.id)),
      reason => current && notify(reason),
    );
    return () => {
      current = false;
    };
  }, [repository]);
  useEffect(() => {
    active.current = true;
    ++sequence.current;
    void refresh();
    const timer = window.setInterval(() => {
      if (!document.hidden) void refresh();
    }, 3000);
    return () => {
      active.current = false;
      window.clearInterval(timer);
    };
  }, [refresh]);

  async function mutate(operation: Operation) {
    const stop = operation.case === "stop";
    const pending = stop ? stopPending : commandPending;
    if (pending.current || (!stop && (preparing.current || stopPending.current || !fresh)))
      return false;
    pending.current = true;
    (stop ? setStopping : setBusy)(true);
    const current = ++sequence.current;
    try {
      const next = await strategyRequest(account, operation);
      if (active.current && current === sequence.current) accept(next);
      return true;
    } catch (reason) {
      if (active.current && current === sequence.current) {
        notify(reason, { namespace: "asterion.terminal.trading" });
        setFresh(false);
      }
      return false;
    } finally {
      pending.current = false;
      if (active.current) (stop ? setStopping : setBusy)(false);
    }
  }
  async function perform(action: () => Promise<void>) {
    if (preparing.current || commandPending.current) return;
    preparing.current = true;
    setBusy(true);
    try {
      await action();
    } catch (reason) {
      notify(reason, { namespace: "asterion.terminal.trading" });
    } finally {
      preparing.current = false;
      setBusy(false);
    }
  }
  return (
    <section className="account-setup" aria-label={t("策略包")}>
      <div className="workflow-heading">
        <h3>{t("策略包")}</h3>
        <button disabled={reading || busy || stopping} onClick={() => void refresh()}>
          {t("读取主机策略包")}
        </button>
      </div>
      <p className="subtle">{t("发布只安装代码，不启动策略或授权交易。")}</p>
      {!fresh && (
        <p role="status">
          {t(
            state
              ? "状态未确认，以下为上次已知状态。请读取主机状态后再启动或发布。"
              : readError
                ? "策略状态不可用，请重新读取。"
                : "正在读取交易主机策略状态。",
          )}
        </p>
      )}
      {readError && (
        <p className="alert" role="alert">
          <ErrorNotice error={readError} namespace="asterion.terminal.trading" />
        </p>
      )}
      {!repository ? (
        <p>{t("Terminal 没有策略目录：从项目启动时是项目里的 research/。")}</p>
      ) : (
        <>
          <label>
            {t("策略")}
            <select
              value={strategy}
              disabled={busy}
              onChange={event => {
                setStrategy(event.target.value);
                setPreparation(undefined);
              }}
            >
              <option value="">{t("选择策略")}</option>
              {strategies.map(id => (
                <option key={id} value={id}>
                  {id}
                </option>
              ))}
            </select>
          </label>
          <button
            disabled={busy || !strategy}
            onClick={() =>
              void perform(async () => {
                setPreparation(undefined);
                const value = await lab(
                  { case: "strategyPackage", value: { repository, strategy } },
                  "strategyPackage",
                );
                setPreparation({ repository, value });
              })
            }
          >
            {t("准备策略包")}
          </button>
        </>
      )}
      {prepared?.manifest && (
        <div>
          <p>
            <strong>
              {prepared.manifest.name} · {prepared.manifest.version}
            </strong>{" "}
            · {prepared.manifest.entry}
          </p>
          <p>{prepared.manifest.files.join(", ")}</p>
          <button
            disabled={busy || stopping || !fresh}
            onClick={() =>
              void mutate({ case: "publish", value: prepared }).then(published => {
                if (published) setPreparation(undefined);
              })
            }
          >
            {t("发布到此交易主机")}
          </button>
        </div>
      )}
      {state && (
        <>
          <ul>
            {state.packages.map(item => (
              <li key={`${item.name}/${item.version}`}>
                {item.name} · {item.version} · {item.entry}
              </li>
            ))}
          </ul>
          {!state.packages.length && <p>{t("此账户尚未安装策略包。")}</p>}
          <StrategyMonitor
            instance={state.instance}
            observedAt={observedAt}
            fresh={fresh}
            stopping={stopping}
            busy={busy}
            stop={instanceId => mutate({ case: "stop", value: { instanceId } })}
            pause={(instanceId, paused) => mutate({ case: "pause", value: { instanceId, paused } })}
            flatten={instanceId => mutate({ case: "flatten", value: { instanceId } })}
          />
          <StrategyInstance
            state={state}
            live={live}
            available={fresh && connected}
            busy={busy || stopping}
            start={configuration => mutate({ case: "start", value: configuration })}
          />
        </>
      )}
    </section>
  );
}
