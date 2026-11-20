import { listSeparator } from "../../src/i18n";
import { useEffect, useState } from "react";
import type { Package } from "../../src/generated/asterion/strategy_pb";
import { lab } from "../../src/bridge/lab";
import { ActionError } from "../../src/host/action-error";
import { notify, useErrorNotification } from "../../src/host/notifications";
import { t } from "./live-shared";
import type { StrategyHost } from "../../src/host/trading-accounts";

// Packages of the account's host: prepare from research and publish.
export function StrategyPackages({
  host,
  repository,
  initialStrategy,
  initialRun,
  openRun,
}: {
  host: StrategyHost;
  repository: string;
  initialStrategy?: string;
  initialRun?: string;
  openRun?: (id: string) => void;
}) {
  const { state, busy, stopping, reading, fresh, readError, error, refresh, mutate, perform } =
    host;
  const [strategies, setStrategies] = useState<string[]>([]);
  const [strategy, setStrategy] = useState(initialStrategy ?? "");
  const [run, setRun] = useState(initialRun ?? "");
  const [preparation, setPreparation] = useState<{ repository: string; value: Package }>();
  const prepared = preparation?.repository === repository ? preparation.value : undefined;
  useEffect(() => {
    if (initialStrategy) {
      setStrategy(initialStrategy);
      setPreparation(undefined);
    }
  }, [initialStrategy]);
  useEffect(() => {
    if (!repository || run) return;
    let current = true;
    void lab({ case: "listStrategies", value: { path: repository } }).then(
      listed => current && setStrategies(listed.strategies.map(item => item.id)),
      reason => current && notify(reason),
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
        <button disabled={reading || busy || stopping} onClick={() => void refresh()}>
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
          <p>
            {t("来源回测")} · {run}
          </p>
          <button
            disabled={busy}
            onClick={() =>
              void perform(async () => {
                setPreparation(undefined);
                const value = await lab({ case: "buildPackage", value: { runId: run } });
                setPreparation({ repository, value });
              })
            }
          >
            {t("生成版本")}
          </button>
          <button
            disabled={busy}
            onClick={() => {
              setRun("");
              setPreparation(undefined);
            }}
          >
            {t("改用当前源码")}
          </button>
        </div>
      ) : !repository ? (
        <p>{t("没有策略目录")}</p>
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
            {t("生成版本")}
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
          <p>{prepared.manifest.files.join(listSeparator())}</p>
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
          {state.packages.length ? (
            <table aria-label={t("已发布的版本")}>
              <thead>
                <tr>
                  <th scope="col">{t("名称")}</th>
                  <th scope="col">{t("版本")}</th>
                  <th scope="col">{t("入口")}</th>
                  <th scope="col">{t("来源回测")}</th>
                </tr>
              </thead>
              <tbody>
                {state.packages.map(item => (
                  <tr key={`${item.name}/${item.version}`}>
                    <td>{item.name}</td>
                    <td>{item.version}</td>
                    <td>{item.entry}</td>
                    <td>
                      {item.research && openRun ? (
                        <button className="link" onClick={() => openRun(item.research!.runId)}>
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
            <p>{t("此账户还没有发布过策略版本。")}</p>
          )}
        </>
      )}
    </section>
  );
}
