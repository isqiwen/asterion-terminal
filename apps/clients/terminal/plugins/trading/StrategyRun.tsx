import { useState } from "react";
import { diagnosticSummary, translate, type MessageValues } from "../contract";
import type { LiveSession } from "../../src/bridge/client";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);
type Contract = LiveSession["contracts"][number];
const key = (item: Contract) => `${item.venue}:${item.symbol}`;

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
  const missing = Math.max(run.slow - run.bars, 0);
  return (
    <section className="strategy-run" aria-label={t("策略运行")}>
      <div className="strategy-run-heading">
        <strong>{t("策略运行中")}</strong>
        <span>
          {t("{venue} · {symbol} · 均线 {fast}/{slow} · {quantity} 手", {
            venue: run.venue,
            symbol: run.symbol,
            fast: run.fast,
            slow: run.slow,
            quantity: run.quantity,
          })}
        </span>
        <button disabled={stopping} onClick={onStop}>
          {t("停止策略并撤单")}
        </button>
      </div>
      <dl className="strategy-run-facts">
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
          <dd data-testid="strategy-target">
            {run.target === null ? "—" : t("{n} 手", { n: run.target })}
          </dd>
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

// Hands the authorized account to one moving-average run on an allowed contract.
export function StrategyStart({
  live,
  busy,
  onStart,
}: {
  live: LiveSession;
  busy: boolean;
  onStart: (params: Record<string, unknown>) => void;
}) {
  const [form, setForm] = useState({ contract: "", fast: "5", slow: "20", quantity: "1" });
  const chosen = live.contracts.find(item => key(item) === form.contract) ?? live.contracts[0];
  const last = live.strategy;
  return (
    <form
      aria-label={t("启动策略")}
      onSubmit={event => {
        event.preventDefault();
        if (!chosen) return;
        onStart({
          venue: chosen.venue,
          symbol: chosen.symbol,
          fast: Number(form.fast),
          slow: Number(form.slow),
          quantity: form.quantity,
        });
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
        <div className="futures-fields">
          <label>
            {t("合约")}
            <select
              aria-label={t("策略合约")}
              value={chosen ? key(chosen) : ""}
              onChange={event => setForm({ ...form, contract: event.target.value })}
            >
              {live.contracts.map(item => (
                <option key={key(item)} value={key(item)}>
                  {item.venue} · {item.symbol}
                </option>
              ))}
            </select>
          </label>
          {(
            [
              ["fast", "快线周期"],
              ["slow", "慢线周期"],
              ["quantity", "持仓手数"],
            ] as const
          ).map(([field, label]) => (
            <label key={field}>
              {t(label)}
              <input
                aria-label={t(label)}
                inputMode="numeric"
                pattern="[1-9][0-9]*"
                value={form[field]}
                onChange={event => setForm({ ...form, [field]: event.target.value })}
                required
              />
            </label>
          ))}
        </div>
        <div className="source-actions">
          <button className="primary" type="submit">
            {t("启动策略")}
          </button>
          <span className="subtle">
            {t(
              "均线多头策略：快线高于慢线时持有上面的手数，否则空仓；使用本机行情服务的 1 分钟线，按 K 线收盘价挂限价单。启动后由策略独占下单，断线重连和换交易日后自动继续，直到你停止它、撤销授权、修改风控政策或交易服务重启。",
            )}
          </span>
        </div>
      </fieldset>
    </form>
  );
}
