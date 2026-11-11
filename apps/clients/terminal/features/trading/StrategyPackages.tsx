import { useCallback, useEffect, useRef, useState } from "react";
import type { Package } from "../../src/generated/asterion/strategy_pb";
import type { Account, Strategies } from "../../src/generated/asterion/trading_pb";
import { lab } from "../../src/bridge/lab";
import { strategyRequest, type StrategyCall } from "../../src/bridge/strategy";
import { ActionError, useActionError } from "../../src/host/action-error";
import { notify } from "../../src/host/notifications";
import { type DisplayError, ErrorNotice, asDisplayError } from "../../src/i18n/errors";
import { t } from "./live-shared";
import { StrategyInstance } from "./StrategyInstance";
import { StrategyMonitor } from "./StrategyMonitor";
import "./strategy.css";

type Props = { account: string; repository: string; record?: Account; connected?: boolean };

export function StrategyPackages(props: Props) {
  return <AccountStrategies key={props.account} {...props} />;
}

function AccountStrategies({ account, repository, record, connected = false }: Props) {
  const [strategies, setStrategies] = useState<string[]>([]);
  const [strategy, setStrategy] = useState("");
  const [preparation, setPreparation] = useState<{ repository: string; value: Package }>();
  const prepared = preparation?.repository === repository ? preparation.value : undefined;
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
    if (!repository) return;
    let current = true;
    void lab({ case: "listStrategies", value: { path: repository } }).then(
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
  return (
    <section className="ctp-strategies" aria-label={t("策略包")}>
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
      <ActionError error={error} namespace="asterion.terminal.trading" />
      {!repository ? (
        <p>{t("Terminal 没有策略目录：从项目启动时是项目里的 research/。")}</p>
      ) : (
        <div>
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
                const value = await lab({ case: "buildPackage", value: { repository, strategy } });
                setPreparation({ repository, value });
              })
            }
          >
            {t("准备策略包")}
          </button>
        </div>
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
              void mutate({ case: "publishStrategy", value: prepared }).then(published => {
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
            stop={id => mutate({ case: "stopStrategy", value: { id } })}
            pause={(id, paused) => mutate({ case: "pauseStrategy", value: { id, paused } })}
            flatten={id => mutate({ case: "flattenStrategy", value: { id } })}
          />
          <StrategyInstance
            state={state}
            account={record}
            available={fresh && connected}
            busy={busy || stopping}
            start={configuration => mutate({ case: "startStrategy", value: configuration })}
          />
        </>
      )}
    </section>
  );
}
