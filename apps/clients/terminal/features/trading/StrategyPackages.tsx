import { useCallback, useEffect, useRef, useState } from "react";
import type { Package, State } from "../../src/generated/asterion/strategy_pb";
import { lab } from "../../src/bridge/lab";
import { strategyRequest } from "../../src/bridge/strategy";
import { ErrorNotice, asDisplayError, type DisplayError } from "../contract";
import { t } from "./live-shared";
import { StrategyInstance, type StrategyAccount } from "./StrategyInstance";
import { StrategyMonitor } from "./StrategyMonitor";
import "./strategy.css";

type Props = { account: string; repository: string; live?: StrategyAccount; connected?: boolean };
type Operation = NonNullable<Parameters<typeof strategyRequest>[1]>;

export function StrategyPackages(props: Props) {
  return <AccountStrategies key={props.account} {...props} />;
}

function AccountStrategies({ account, repository, live, connected = false }: Props) {
  const [manifest, setManifest] = useState("strategy.json");
  const [preparation, setPreparation] = useState<{ repository: string; value: Package }>();
  const prepared = preparation?.repository === repository ? preparation.value : undefined;
  const [state, setState] = useState<State>();
  const [busy, setBusy] = useState(false);
  const [stopping, setStopping] = useState(false);
  const [reading, setReading] = useState(false);
  const [fresh, setFresh] = useState(false);
  const [observedAt, setObservedAt] = useState<Date>();
  const [readError, setReadError] = useState<DisplayError>("");
  const [error, setError] = useState<DisplayError>("");
  const sequence = useRef(0);
  const active = useRef(true);
  const readPending = useRef(false);
  const commandPending = useRef(false);
  const stopPending = useRef(false);
  const preparing = useRef(false);
  const accept = (next: State) => {
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
    setError("");
    const current = ++sequence.current;
    try {
      const next = await strategyRequest(account, operation);
      if (active.current && current === sequence.current) accept(next);
      return true;
    } catch (reason) {
      if (active.current && current === sequence.current) {
        setError(asDisplayError(reason));
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
    setError("");
    try {
      await action();
    } catch (reason) {
      setError(asDisplayError(reason));
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
      {error && (
        <p className="alert" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.trading" />
        </p>
      )}
      {!repository ? (
        <p>{t("请先在研究设置中选择策略仓库。")}</p>
      ) : (
        <>
          <label>
            {t("包描述文件")}
            <input
              value={manifest}
              disabled={busy}
              onChange={event => {
                setManifest(event.target.value);
                setPreparation(undefined);
              }}
            />
          </label>
          <button
            disabled={busy || !manifest}
            onClick={() =>
              void perform(async () => {
                setPreparation(undefined);
                const value = await lab(
                  { case: "strategyPackage", value: { repository, manifest } },
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
            stop={instanceId => mutate({ case: "stop", value: { instanceId } })}
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
