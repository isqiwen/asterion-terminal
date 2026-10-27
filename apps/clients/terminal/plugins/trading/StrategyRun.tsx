import { useCallback, useEffect, useRef, useState } from "react";
import {
  asDisplayError,
  diagnosticSummary,
  ErrorNotice,
  strategyRule,
  strategySides,
  strategySize,
  translate,
  type DisplayError,
  type MessageValues,
  type TerminalContext,
} from "../contract";
import type { LiveSession, ResearchStrategyPackage } from "../../src/bridge/client";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
// A package a live run can trade: one contract on completed one-minute bars.
type LivePackage = ResearchStrategyPackage & {
  definition: Extract<ResearchStrategyPackage["definition"], { strategy: object }>;
};
const isLivePackage = (item: ResearchStrategyPackage): item is LivePackage =>
  !item.retirement_reason &&
  item.definition.interval_minutes === 1 &&
  Boolean(item.definition.strategy);
// A signed target in lots, said the way a position is: long, short or flat.
const position = (target: string) =>
  target === "0"
    ? t("空仓")
    : target.startsWith("-")
      ? t("空 {n} 手", { n: target.slice(1) })
      : t("多 {n} 手", { n: target });
const key = (item: { venue: string; symbol: string }) => `${item.venue}:${item.symbol}`;

// The account's strategy run. While it runs it is the only originator of new
// orders, so this replaces the order ticket; stopping it is always offered.
export function StrategyStatus({
  live,
  stopping,
  onStop,
}: {
  live: LiveSession;
  stopping: boolean;
  onStop: () => void;
}) {
  const run = live.strategy!;
  const missing = Math.max(run.warmup - run.bars, 0);
  return (
    <section className="strategy-run" aria-label={t("策略运行")}>
      <div className="strategy-run-heading">
        <strong>{t("策略运行中")}</strong>
        <span>
          {t("{venue} · {symbol} · {rule} · {size} · {sides}", {
            venue: run.venue,
            symbol: run.symbol,
            rule: strategyRule(run.strategy),
            size: strategySize(run.strategy),
            sides: strategySides(run.strategy.sides),
          })}
        </span>
        <button disabled={stopping} onClick={onStop}>
          {t("停止策略并撤单")}
        </button>
      </div>
      <dl className="strategy-run-facts">
        <div>
          <dt>{t("策略包")}</dt>
          <dd data-testid="strategy-package">{run.package.id}</dd>
        </div>
        <div>
          <dt>{t("1 分钟线")}</dt>
          <dd data-testid="strategy-bars">
            {run.target === null && missing > 0
              ? t("已收到 {n} 根，还需 {missing} 根才有信号", { n: run.bars, missing })
              : t("已收到 {n} 根", { n: run.bars })}
          </dd>
        </div>
        <div>
          <dt>{t("目标持仓")}</dt>
          <dd data-testid="strategy-target">{run.target === null ? "—" : position(run.target)}</dd>
        </div>
        <div>
          <dt>{t("策略挂单")}</dt>
          <dd>{run.orders.length ? run.orders.join("、") : t("无")}</dd>
        </div>
      </dl>
      <p className="subtle">
        {t(
          "策略运行期间不能手动下单；撤单、撤销授权和停止策略始终可用。账户未连接或未同步时策略等待，不发送委托。停止只撤销策略的挂单，持仓保留。",
        )}
      </p>
    </section>
  );
}

// Hands the authorized account to one run of a one-minute strategy package
// whose contract the account's policy allows.
export function StrategyStart({
  live,
  busy,
  query,
  onStart,
}: {
  live: LiveSession;
  busy: boolean;
  query: TerminalContext["query"];
  onStart: (packageId: string) => void;
}) {
  const [packages, setPackages] = useState<LivePackage[] | null>(null);
  const [error, setError] = useState<DisplayError | null>(null);
  const [selected, setSelected] = useState("");
  const ask = useRef(query);
  ask.current = query;
  const load = useCallback(async () => {
    setError(null);
    try {
      const value = (await ask.current("research.strategy.list", { after: "", limit: 200 }))
        .research_strategy;
      setPackages((value?.packages ?? []).filter(isLivePackage));
    } catch (failure) {
      setError(asDisplayError(failure));
    }
  }, []);
  useEffect(() => {
    void load();
  }, [load]);
  const usable = (packages ?? []).filter(item =>
    live.contracts.some(contract => key(contract) === key(item.definition.contract)),
  );
  const chosen = usable.find(item => item.object.id === selected) ?? usable[0];
  const last = live.strategy;
  return (
    <form
      aria-label={t("启动策略")}
      onSubmit={event => {
        event.preventDefault();
        if (chosen) onStart(chosen.object.id);
      }}
    >
      <fieldset disabled={busy}>
        {last && (
          <p className="subtle" data-testid="strategy-ended">
            {t("上次运行已停止：{reason}", {
              reason: diagnosticSummary(last.reason) ?? last.reason,
            })}
          </p>
        )}
        {error && <ErrorNotice error={error} namespace="asterion.terminal.trading" />}
        {packages && !chosen && (
          <p className="subtle" data-testid="strategy-packages-empty">
            {t(
              "没有可运行的策略包。实盘运行只接受 1 分钟单合约策略包，且合约须在账户风控允许的合约中；请先在研究中用 1 分钟数据建立策略包。",
            )}
          </p>
        )}
        {chosen && (
          <>
            <div className="futures-fields">
              <label>
                {t("策略包")}
                <select
                  aria-label={t("运行的策略包")}
                  value={chosen.object.id}
                  onChange={event => setSelected(event.target.value)}
                >
                  {usable.map(item => (
                    <option key={item.object.id} value={item.object.id}>
                      {item.name} · {key(item.definition.contract)}
                    </option>
                  ))}
                </select>
              </label>
            </div>
            <p data-testid="strategy-package-rule">
              {t("{rule} · {size} · {sides}", {
                rule: strategyRule(chosen.definition.strategy),
                size: strategySize(chosen.definition.strategy),
                sides: strategySides(chosen.definition.strategy.sides),
              })}
            </p>
          </>
        )}
        <div className="source-actions">
          <button className="primary" type="submit" disabled={!chosen}>
            {t("启动策略")}
          </button>
          <button type="button" onClick={() => void load()}>
            {t("刷新策略包")}
          </button>
          <span className="subtle">
            {t(
              "策略按策略包固定的规则运行，使用本机行情服务的 1 分钟线，按 K 线收盘价挂限价单；该合约已有的单边持仓由策略接管。启动后由策略独占下单，断线重连和换交易日后自动继续，直到你停止它、撤销授权、修改风控政策或交易服务重启。",
            )}
          </span>
        </div>
      </fieldset>
    </form>
  );
}
