import type { ReactNode } from "react";
import { translate, listSeparator, type MessageValues } from "../../src/i18n";
import { RunState, type RunSummary } from "../../src/generated/asterion/lab_pb";
import { Environment, InstanceState } from "../../src/generated/asterion/trading_pb";
import { useTradingAccounts } from "../../src/host/trading-accounts";
import { metric, signed } from "./ResearchResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);
const running = new Set([InstanceState.STARTING, InstanceState.WARMING, InstanceState.READY]);

function Step({ label, done, children }: { label: string; done: boolean; children: ReactNode }) {
  return (
    <li data-done={done || undefined}>
      <span>{t(label)}</span>
      {children}
    </li>
  );
}

function Link({ onClick, children }: { onClick: () => void; children: ReactNode }) {
  return (
    <button type="button" className="lab-link" onClick={onClick}>
      {children}
    </button>
  );
}

// Where one strategy stands on its way to trading: its code, its latest
// successful backtest, the latest version published to a trading account, and
// the simulation and real accounts that run it. Each step not yet taken names
// the next one.
export function StrategyLifecycle({
  strategy,
  path,
  unsaved,
  runs,
  openCode,
  backtest,
  openRun,
  deploy,
  openTrading,
}: {
  strategy: string;
  path: string;
  unsaved: boolean;
  runs: readonly RunSummary[];
  openCode: () => void;
  backtest: () => void;
  openRun: (id: string) => void;
  deploy: (run: string) => void;
  openTrading: (account?: string) => void;
}) {
  const { views } = useTradingAccounts();
  const run = runs
    .filter(item => item.strategy === strategy && item.state === RunState.SUCCEEDED)
    .sort((a, b) => Number(b.startedMs - a.startedMs))[0];
  let version = "";
  const published: { id: string; name: string }[] = [];
  const publishedRuns = new Set<string>();
  const simulating: { id: string; name: string }[] = [];
  const live: { id: string; name: string }[] = [];
  const realAccounts: string[] = [];
  for (const [id, view] of Object.entries(views)) {
    const name = view.instance || view.account?.id || id;
    if (view.account?.environment === Environment.REAL) realAccounts.push(id);
    for (const item of view.strategies?.packages ?? []) {
      if (item.name !== strategy) continue;
      if (item.research?.runId) publishedRuns.add(item.research.runId);
      if (item.version > version) published.length = 0;
      if (item.version >= version) {
        version = item.version;
        if (!published.some(entry => entry.id === id)) published.push({ id, name });
      }
    }
    const instance = view.strategies?.instance;
    if (instance?.instance?.packageName === strategy && running.has(instance.state))
      (view.account?.environment === Environment.REAL ? live : simulating).push({ id, name });
  }
  const simulationTarget =
    published.find(item => views[item.id]?.account?.environment !== Environment.REAL)?.id ??
    published[0]?.id;
  const hint = (text: string) => <span className="lifecycle-hint">{t(text)}</span>;
  return (
    <ol className="strategy-lifecycle" aria-label={t("上线进度")}>
      <Step label="代码" done={!unsaved}>
        <Link onClick={openCode}>
          {path.replace(/^strategies\//, "")}
          {unsaved && <span className="lifecycle-unsaved"> · {t("未保存")}</span>}
        </Link>
      </Step>
      <Step label="回测" done={!!run}>
        {run ? (
          <Link onClick={() => openRun(run.id)}>
            <span className={signed(run.metrics.total_return)}>
              {metric(run.metrics.total_return, true)}
            </span>
          </Link>
        ) : (
          <Link onClick={backtest}>{t("运行回测")}</Link>
        )}
      </Step>
      <Step label="版本" done={!!version}>
        {version ? (
          <>
            <Link onClick={() => openTrading(published[0]?.id)}>
              {`${version} · ${published.map(item => item.name).join(listSeparator())}`}
            </Link>
            {run && !publishedRuns.has(run.id) && (
              <Link onClick={() => deploy(run.id)}>{t("部署新版")}</Link>
            )}
          </>
        ) : run ? (
          <Link onClick={() => deploy(run.id)}>{t("去部署")}</Link>
        ) : (
          hint("回测后部署")
        )}
      </Step>
      <Step label="仿真" done={simulating.length > 0}>
        {simulating.length ? (
          <Link onClick={() => openTrading(simulating[0]?.id)}>
            {simulating.map(item => item.name).join(listSeparator())}
          </Link>
        ) : version ? (
          <Link onClick={() => openTrading(simulationTarget)}>{t("去启动")}</Link>
        ) : (
          hint("部署后仿真")
        )}
      </Step>
      <Step label="实盘" done={live.length > 0}>
        {live.length ? (
          <Link onClick={() => openTrading(live[0]?.id)}>
            {live.map(item => item.name).join(listSeparator())}
          </Link>
        ) : simulating.length ? (
          <Link onClick={() => openTrading(realAccounts[0])}>{t("去启动")}</Link>
        ) : (
          hint("仿真后上线")
        )}
      </Step>
    </ol>
  );
}
