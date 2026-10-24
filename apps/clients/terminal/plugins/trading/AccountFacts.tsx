import { useEffect, useRef, useState } from "react";
import type { TradingFactsStatus } from "../../src/bridge/client";
import {
  translate,
  getLocale,
  asDisplayError,
  ErrorNotice,
  useWorkspaceDraft,
  type DisplayError,
  type TerminalContext,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.trading", key);
const time = (ms: string) =>
  new Date(Number(ms)).toLocaleString(getLocale(), { timeZone: "Asia/Shanghai", hour12: false });
const gaps: Record<string, string> = {
  cashflow_history: "缺少完整出入金历史",
  settlement_evidence: "缺少结算核对依据",
  valuation_history: "缺少历史估值路径",
  component_business_times: "缺少各分项业务时间",
  complete_broker_history: "不具备完整柜台历史覆盖",
  funds_currency: "缺少资金币种来源字段，金额仅为柜台原值",
};
const phases: Record<string, string> = {
  disconnected: "未连接",
  connecting: "连接前置",
  authenticating: "终端认证",
  logging_in: "登录中",
  confirming: "确认结算单",
  synchronizing: "同步账户",
  ready: "已就绪",
  error: "连接失败",
};
export function AccountFacts({
  context,
  account,
  scope,
}: {
  context: TerminalContext;
  account: string;
  scope: string;
}) {
  const [id, setId] = useWorkspaceDraft(`account-facts-operation:${scope}`, "");
  const [consent, setConsent] = useState(false);
  const [status, setStatus] = useState<TradingFactsStatus | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  useEffect(() => {
    ++epoch.current.value;
    setStatus(null);
    setConsent(false);
    setPending(false);
    setError("");
  }, [scope, id]);
  useEffect(() => {
    const token = epoch.current;
    return () => {
      ++token.value;
    };
  }, []);
  async function run(action: () => Promise<import("../../src/bridge/client").Snapshot>) {
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    try {
      const reply = await action();
      const value = reply.trading_facts;
      if (!value || value.id !== id || (value.capture && value.capture.account_id !== account))
        throw Error(t("固定事实响应身份不符，请检查原操作"));
      if (current === epoch.current.value) setStatus(value);
    } catch (reason) {
      if (current === epoch.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  const shown = status?.capture;
  return (
    <section aria-label={t("固定账户事实")}>
      <h3>{t("固定账户事实")}</h3>
      <p>
        {t(
          "保存当前已观察的账户事实，不查询或登录柜台，不改变授权，不发送委托。它不是完整历史账本。",
        )}
      </p>
      <label>
        {t("账户事实操作 ID")}
        <input value={id} disabled={pending} onChange={event => setId(event.target.value)} />
      </label>
      <button disabled={pending} onClick={() => setId(crypto.randomUUID())}>
        {t("新建观察 ID")}
      </button>
      <label>
        <input
          type="checkbox"
          checked={consent}
          disabled={pending}
          onChange={event => setConsent(event.target.checked)}
        />
        {t("确认保留本次固定事实及覆盖限制")}
      </label>
      <button
        disabled={pending || !id || !consent}
        onClick={() =>
          void run(async () => {
            await context.trade("live.facts.capture", { account, id });
            return context.query("live.facts.status", { account, id });
          })
        }
      >
        {t("固定或恢复原操作")}
      </button>
      <button
        disabled={pending || !id}
        onClick={() => void run(() => context.query("live.facts.status", { account, id }))}
      >
        {t("检查原事实操作")}
      </button>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.trading" />
        </p>
      )}
      {status && (
        <section aria-label={t("固定事实状态")}>
          <p>{t(status.state === "published" ? "账户事实已发布" : "账户事实已接纳，尚未发布")}</p>
          <code>
            {status.reference.node}/{status.reference.service}/{status.reference.version}
          </code>
          <p>
            {t("事实接纳时间（上海）")}：{time(status.admitted_at_ms)}
          </p>
          {status.published_at_ms && (
            <p>
              {t("事实发布时间（上海）")}：{time(status.published_at_ms)}
            </p>
          )}
          {status.state === "admitted" && (
            <p>{t("继续只恢复原件；原件丢失时保持未完成，不重新获取当前观察。")}</p>
          )}
          {status.state === "published" && (
            <button
              disabled={pending}
              onClick={() =>
                void run(() =>
                  context.query("live.facts.read", { account, reference: status.reference }),
                )
              }
            >
              {t("读取此固定事实")}
            </button>
          )}
        </section>
      )}
      {shown && (
        <section aria-label={t("固定账户观察原件")}>
          <p>
            {shown.account_id} · {t(shown.environment === "real" ? "真实资金" : "CTP 仿真")}
          </p>
          <p>
            {t("账户观察取得时间（上海）")}：{time(shown.captured_at_ms)}
          </p>
          <p>
            {t("资金、持仓、订单和成交可能来自不同时间；同步完成时间不代表原子账本或完整历史。")}
          </p>
          <dl>
            <dt>{t("固定观察来源状态")}</dt>
            <dd>{t(phases[shown.phase])}</dd>
            <dt>{t("最近查询完成时间（上海）")}</dt>
            <dd>
              {shown.synchronized_ms === "0" ? t("从未完成查询") : time(shown.synchronized_ms)}
            </dd>
            {shown.error_code !== 0 && (
              <>
                <dt>{t("固定来源错误码")}</dt>
                <dd>{shown.error_code}</dd>
              </>
            )}
            <dt>{t("连接代次")}</dt>
            <dd>{shown.connection_generation}</dd>
            <dt>{t("观察序号")}</dt>
            <dd>{shown.broker_sequence}</dd>
            <dt>{t("持仓核对状态")}</dt>
            <dd>{t(shown.positions_reconciled ? "已纳入已知成交" : "尚未核对已知成交")}</dd>
            <dt>{t("固定持仓数量")}</dt>
            <dd>{shown.positions.length}</dd>
            <dt>{t("固定订单数量")}</dt>
            <dd>{shown.orders.length}</dd>
            <dt>{t("固定成交数量")}</dt>
            <dd>{shown.trades.length}</dd>
          </dl>
          {shown.funds ? (
            <dl aria-label={t("固定资金观察")}>
              <dt>{t("固定余额观察")}</dt>
              <dd>{shown.funds.balance}</dd>
              <dt>{t("固定可用资金")}</dt>
              <dd>{shown.funds.available}</dd>
              <dt>{t("固定保证金")}</dt>
              <dd>{shown.funds.margin}</dd>
              <dt>{t("固定手续费观察")}</dt>
              <dd>{shown.funds.commission}</dd>
              <dt>{t("固定平仓盈亏观察")}</dt>
              <dd>{shown.funds.close_profit}</dd>
              <dt>{t("固定持仓盈亏观察")}</dt>
              <dd>{shown.funds.position_profit}</dd>
            </dl>
          ) : (
            <p>{t("未取得资金观察，不能当作零余额")}</p>
          )}
          <ul>
            {shown.missing_evidence.map(gap => (
              <li key={gap}>{t(gaps[gap] ?? gap)}</li>
            ))}
          </ul>
          <p>{t("固定事实只用于只读证据审阅；尚未提供真实账本绩效计算。")}</p>
          <button
            onClick={() =>
              context.navigate("workspace.account-review", {
                page: "research",
                params: {
                  source: JSON.stringify(shown.reference),
                  account: shown.account_id,
                  environment: shown.environment,
                },
              })
            }
          >
            {t("在 Research 审阅此固定事实")}
          </button>
        </section>
      )}
    </section>
  );
}
