import { listSeparator } from "../../src/i18n";
import { useEffect, useState } from "react";
import type { Package } from "../../src/generated/asterion/strategy_pb";
import { lab } from "../../src/bridge/lab";
import { ActionError } from "../../src/host/action-error";
import { notify } from "../../src/host/notifications";
import { ErrorNotice } from "../../src/i18n/errors";
import { t } from "./live-shared";
import type { StrategyHost } from "./strategy-host";
import "./strategy.css";

// Packages of the account's host: prepare from research and publish.
export function StrategyPackages({ host, repository }: { host: StrategyHost; repository: string }) {
  const { state, busy, stopping, reading, fresh, readError, error, refresh, mutate, perform } =
    host;
  const [strategies, setStrategies] = useState<string[]>([]);
  const [strategy, setStrategy] = useState("");
  const [preparation, setPreparation] = useState<{ repository: string; value: Package }>();
  const prepared = preparation?.repository === repository ? preparation.value : undefined;
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
  return (
    <section className="ctp-strategies" aria-label={t("策略包")}>
      <div className="workflow-heading">
        <h3>{t("策略包")}</h3>
        <button disabled={reading || busy || stopping} onClick={() => void refresh()}>
          {t("读取主机策略包")}
        </button>
      </div>
      {!fresh && (
        <p role="status">
          {t(
            state ? "状态未确认，显示的是上次读到的状态" : readError ? "策略状态不可用" : "读取中",
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
            <table aria-label={t("已发布的策略包")}>
              <thead>
                <tr>
                  <th>{t("名称")}</th>
                  <th>{t("版本")}</th>
                  <th>{t("入口")}</th>
                </tr>
              </thead>
              <tbody>
                {state.packages.map(item => (
                  <tr key={`${item.name}/${item.version}`}>
                    <td>{item.name}</td>
                    <td>{item.version}</td>
                    <td>{item.entry}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : (
            <p>{t("此账户尚未安装策略包。")}</p>
          )}
        </>
      )}
    </section>
  );
}
