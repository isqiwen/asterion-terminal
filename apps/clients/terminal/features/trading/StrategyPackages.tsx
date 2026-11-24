import { useEffect, useRef, useState } from "react";
import type { Package } from "../../src/generated/asterion/strategy_pb";
import { lab } from "../../src/bridge/lab";
import { ActionError } from "../../src/host/action-error";
import { notify, useErrorNotification } from "../../src/host/notifications";
import { t } from "./live-shared";
import type { StrategyHost } from "../../src/host/trading-accounts";

// The versions on the account's host; deploying builds one from research and installs it.
export function StrategyPackages({
  host,
  repository,
  initialStrategy,
  initialRun,
  openRun,
  published,
  onDeployFirst,
}: {
  host: StrategyHost;
  repository: string;
  initialStrategy?: string;
  initialRun?: string;
  openRun?: (id: string) => void;
  published?: (key: string) => void;
  // Told whether deploying, rather than starting, is the next step.
  onDeployFirst?: (first: boolean) => void;
}) {
  const { state, busy, stopping, reading, fresh, readError, error, refresh, mutate, perform } =
    host;
  const [strategies, setStrategies] = useState<string[]>([]);
  const [strategy, setStrategy] = useState(initialStrategy ?? "");
  const [run, setRun] = useState(initialRun ?? "");
  useEffect(() => {
    if (initialStrategy) setStrategy(initialStrategy);
  }, [initialStrategy]);
  // A build asked for under another directory, or a closed page, installs nothing.
  const openDirectory = useRef<string | null>(repository);
  useEffect(() => {
    openDirectory.current = repository;
    return () => {
      openDirectory.current = null;
    };
  }, [repository]);
  const deploy = async (source: { runId: string } | { repository: string; strategy: string }) => {
    let built: Package | undefined;
    await perform(async () => {
      built = await lab({ case: "buildPackage", value: source });
    });
    if (!built || openDirectory.current !== repository) return;
    const { name, version } = built.manifest!;
    if (await mutate({ case: "publishStrategy", value: built })) published?.(`${name}/${version}`);
  };
  const unavailable = busy || stopping || !fresh;
  // A backtest's source is frozen, so deploying it again would only add a copy.
  const deployed = !!run && !!state?.packages.some(item => item.research?.runId === run);
  const deployFirst = run ? !deployed : !state?.packages.length;
  useEffect(() => onDeployFirst?.(deployFirst), [deployFirst, onDeployFirst]);
  useEffect(() => {
    if (!repository || run) return;
    let current = true;
    void lab({ case: "listStrategies", value: { path: repository } }).then(
      listed => current && setStrategies(listed.strategies.map(item => item.id)),
      reason => current && notify(reason, { namespace: "asterion.terminal.trading" }),
    );
    return () => {
      current = false;
    };
  }, [repository, run]);
  useErrorNotification(readError, "asterion.terminal.trading");
  return (
    <section className="ctp-strategies" aria-label={t("策略版本")}>
      <div className="workflow-heading">
        <h2>{t("策略版本")}</h2>
        <button type="button" disabled={reading || busy || stopping} onClick={() => void refresh()}>
          {t("刷新版本")}
        </button>
      </div>
      {!fresh && (
        <p role="status">
          {t(
            state ? "状态未确认，显示的是上次读到的状态" : readError ? "策略状态不可用" : "读取中",
          )}
        </p>
      )}
      <ActionError error={error} namespace="asterion.terminal.trading" />
      {run ? (
        <div>
          <p className="strategy-package-source" title={run}>
            {t("来源回测")} · {run}
          </p>
          <div className="strategy-package-actions">
            <button
              type="button"
              className={deployFirst ? "primary" : undefined}
              disabled={unavailable || deployed}
              onClick={() => void deploy({ runId: run })}
            >
              {t(deployed ? "已部署" : "部署")}
            </button>
            <button
              type="button"
              disabled={busy}
              onClick={() => {
                setRun("");
              }}
            >
              {t("改用当前源码")}
            </button>
          </div>
        </div>
      ) : !repository ? (
        <p>{t("没有策略目录")}</p>
      ) : (
        <div className="strategy-package-picker">
          <label>
            {t("策略")}
            <select
              value={strategy}
              disabled={busy}
              onChange={event => setStrategy(event.target.value)}
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
            type="button"
            className={deployFirst ? "primary" : undefined}
            disabled={unavailable || !strategy}
            onClick={() => void deploy({ repository, strategy })}
          >
            {t("部署")}
          </button>
        </div>
      )}
      {state && (
        <>
          {state.packages.length ? (
            <table aria-label={t("已部署的版本")}>
              <thead>
                <tr>
                  <th scope="col">{t("名称")}</th>
                  <th scope="col">{t("版本")}</th>
                  <th scope="col">{t("来源回测")}</th>
                </tr>
              </thead>
              <tbody>
                {state.packages.map(item => (
                  <tr key={`${item.name}/${item.version}`}>
                    <td title={`${item.name}\n${item.entry}`}>
                      <span>{item.name}</span>
                      <small>{item.entry}</small>
                    </td>
                    <td>{item.version}</td>
                    <td title={item.research?.runId}>
                      {item.research && openRun ? (
                        <button
                          type="button"
                          className="link"
                          onClick={() => openRun(item.research!.runId)}
                        >
                          {item.research.runId}
                        </button>
                      ) : (
                        (item.research?.runId ?? "—")
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : (
            <p>{t("此账户还没有部署过策略。")}</p>
          )}
        </>
      )}
    </section>
  );
}
