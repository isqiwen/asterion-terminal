import { HistoryDatasetViewer } from "./HistoryDatasetViewer";
import { useState } from "react";
import { historySources, type HistorySource } from "./history-sources";
import { readableConnection, timestamp } from "../../src/bridge/client";
import { BackendError } from "../../src/i18n/errors";
import {
  type TerminalContext,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  translate,
  type MessageValues,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const states = {
  queued: "排队中",
  running: "下载中",
  cancel_requested: "正在取消",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
  interrupted: "已中断",
};
function SourceDownloads({
  snapshot,
  busy,
  trade,
  query,
  navigate,
  openSettings,
  source,
}: Pick<TerminalContext, "snapshot" | "busy" | "trade" | "query" | "navigate" | "openSettings"> & {
  source: HistorySource;
}) {
  const [form, setForm] = useWorkspaceDraft(`historyQuery:${source.id}`, {
    code: "",
    frequency: String(source.intervals[0]),
    exchange: source.exchanges[0],
    product: "",
    rate: String(source.rate.default),
  });
  const [viewId, setViewId] = useState<string | null>(null);
  const [connectionId, setConnectionId] = useState("");
  const savedConnections =
    snapshot?.data_connections
      ?.filter(readableConnection)
      .filter(item => item.source === source.id) ?? [];
  const connection = savedConnections.find(item => item.id === connectionId);
  const [token, setToken] = useState(""); // Never retain credentials in workspace drafts/storage.
  const [error, setError] = useState<DisplayError>("");
  const research = snapshot?.research;
  const catalog = source.catalog(snapshot);
  const matchingCatalog =
    catalog?.exchange === form.exchange &&
    catalog.product === form.product &&
    catalog.connection === (connection?.id ?? "") &&
    catalog.connection_revision === (connection?.revision ?? "")
      ? catalog
      : null;
  const contracts = matchingCatalog?.items ?? [];
  const selected = contracts.find(item => item.code === form.code);
  // "*" downloads every listed month; at most two downloads run at once, so
  // each gets half the rate and together they stay within it.
  const all = form.code === "*" && contracts.length > 0;
  const targets = all ? contracts : selected ? [selected] : [];
  const [submitted, setSubmitted] = useState<number | null>(null);
  const cutoff = matchingCatalog?.cutoff_ns ?? "0";
  const [pending, setPending] = useWorkspaceRequestId(
    `historySubmission:${source.id}`,
    JSON.stringify([research?.connection_id, form, cutoff, connection?.id, connection?.revision]),
  );
  const tasks = research?.tasks.filter(source.ownsTask) ?? [];
  const result = snapshot?.research_result ? source.dataset(snapshot.research_result) : null;
  const update = (key: keyof typeof form, value: string) =>
    setForm(previous => ({ ...previous, [key]: value }));
  const run = async (method: Parameters<typeof trade>[0], params?: Record<string, unknown>) => {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  };
  if (viewId)
    return (
      <HistoryDatasetViewer
        id={viewId}
        source={source.id}
        sourceLabel={source.name}
        timeAxis={source.timeAxis}
        snapshot={snapshot}
        busy={busy}
        query={query}
        onClose={() => setViewId(null)}
      />
    );
  return (
    <div className="history-layout">
      <section className="history-config" aria-label={t("新建下载")}>
        <h3>{t("新建下载")}</h3>
        <p className="content-caption">{t(source.description)}</p>
        {research?.remote ? (
          <div className="source-actions">
            <p>{t("历史下载当前使用本机研究服务。")}</p>
            <button onClick={() => openSettings("connections")}>{t("查看运行位置")}</button>
          </div>
        ) : null}
        <form
          onSubmit={event => {
            event.preventDefault();
            if (!targets.length || !matchingCatalog) return;
            const id = pending ?? `history-${crypto.randomUUID()}`;
            setPending(id);
            setSubmitted(null);
            const rate = Number(connection ? connection.requests_per_minute : form.rate);
            void (async () => {
              let count = 0;
              for (const [index, item] of targets.entries()) {
                const ok = await run(source.command, {
                  // Stable per contract, so resubmitting the batch is idempotent.
                  id: all ? `${id}-${index}` : id,
                  ...source.parameters(
                    {
                      ...form,
                      code: item.code,
                      rate: String(all ? Math.max(1, Math.floor(rate / 2)) : rate),
                    },
                    connection ? "" : token,
                    matchingCatalog,
                  ),
                  connection: connection?.id ?? "",
                  connection_revision: connection?.revision ?? "",
                });
                if (!ok) break;
                count += 1;
              }
              if (all) setSubmitted(count);
              if (count === targets.length) setPending(null);
            })().finally(() => setToken(""));
          }}
        >
          <fieldset disabled={busy || !research?.online || research.remote}>
            <div className="futures-fields">
              <label>
                {t("交易所")}
                <select
                  aria-label={t("交易所")}
                  value={form.exchange}
                  onChange={event =>
                    setForm(previous => ({ ...previous, exchange: event.target.value, code: "" }))
                  }
                >
                  {source.exchanges.map(exchange => (
                    <option key={exchange}>{exchange}</option>
                  ))}
                </select>
              </label>
              <label>
                {t("品种代码")}
                <input
                  required
                  placeholder="CU"
                  value={form.product}
                  onChange={event =>
                    setForm(previous => ({ ...previous, product: event.target.value, code: "" }))
                  }
                />
              </label>
              <label>
                {t("使用连接")}
                <select
                  aria-label={t("使用连接")}
                  value={connectionId}
                  onChange={event => {
                    setConnectionId(event.target.value);
                    setToken("");
                    setForm(previous => ({ ...previous, code: "" }));
                  }}
                >
                  <option value="">{t("临时凭据")}</option>
                  {savedConnections.map(item => (
                    <option key={item.id} value={item.id}>
                      {item.name}
                    </option>
                  ))}
                </select>
              </label>
              {!connection && (
                <label>
                  {source.credential.label}
                  <input
                    type="password"
                    autoComplete="off"
                    required={source.credential.required}
                    maxLength={source.credential.maxLength}
                    value={token}
                    onChange={event => setToken(event.target.value)}
                  />
                </label>
              )}
              {connection && source.credential.required && !connection.credential_ready && (
                <p>{t("请在设置中重新输入连接凭据。")}</p>
              )}
              <div className="history-catalog-action">
                <button
                  type="button"
                  disabled={
                    (source.credential.required &&
                      !(connection ? connection.credential_ready : token)) ||
                    !form.product
                  }
                  onClick={() =>
                    void run(source.catalogCommand, {
                      source: source.id,
                      exchange: form.exchange,
                      product: form.product,
                      token: connection ? "" : token,
                      connection: connection?.id ?? "",
                      connection_revision: connection?.revision ?? "",
                    })
                  }
                >
                  {t("查询月份合约")}
                </button>
              </div>
              <label>
                {t("月份合约")}
                <select
                  aria-label={t("月份合约")}
                  required
                  value={selected || all ? form.code : ""}
                  onChange={event => update("code", event.target.value)}
                >
                  <option value="">{t("选择具体月份合约")}</option>
                  {contracts.length > 1 && (
                    <option value="*">
                      {t("全部月份合约（{n} 个）", { n: contracts.length })}
                    </option>
                  )}
                  {[...contracts].reverse().map(item => (
                    <option key={item.code} value={item.code}>
                      {item.code} · {item.name}
                    </option>
                  ))}
                </select>
              </label>
              <label>
                {t(source.timeAxis === "trading-day" ? "数据周期" : "分钟周期")}
                <select
                  aria-label={t(source.timeAxis === "trading-day" ? "数据周期" : "分钟周期")}
                  value={form.frequency}
                  onChange={event => update("frequency", event.target.value)}
                >
                  {source.intervals.map(value => (
                    <option key={value} value={value}>
                      {value === "day" ? t("日K") : `${value} min`}
                    </option>
                  ))}
                </select>
              </label>
              <label>
                {t("每分钟请求上限")}
                <input
                  type="number"
                  min="1"
                  max={source.rate.max}
                  required
                  disabled={!!connection}
                  value={connection ? String(connection.requests_per_minute) : form.rate}
                  onChange={event => update("rate", event.target.value)}
                />
              </label>
            </div>
            {selected && (
              <p className="history-lifetime">
                {t("上市日期")} {selected.list_date} — {t("最后交易日")} {selected.delist_date}
                <br />
                {t("自动下载该合约完整存续期；未到期合约截至本次查询时刻。")}
                <br />
                {t("本次截止")} {timestamp(cutoff)}
              </p>
            )}
            {selected && (
              <details className="history-notes">
                <summary>{t("合约计量信息")}</summary>
                <dl className="history-metrics">
                  {[
                    ["供应商合约乘数", selected.multiplier],
                    ["每手交易单位", selected.per_unit],
                    ["交易计量单位", selected.trade_unit],
                    ["报价单位", selected.quote_unit],
                  ].map(([label, value]) => (
                    <div key={label}>
                      <dt>{t(label!)}</dt>
                      <dd>{value ?? "—"}</dd>
                    </div>
                  ))}
                </dl>
                <p className="subtle">
                  {t("保留数据源原始字段；合约乘数与每手交易单位不自动互换，缺失值不推算。")}
                </p>
              </details>
            )}
            {matchingCatalog && !contracts.length && (
              <p role="status">{t("没有找到月份合约，请检查交易所和品种代码。")}</p>
            )}
            <p className="subtle">{t(source.credential.help)}</p>
            {all && (
              <p className="subtle">
                {t(
                  "为每个月份合约各建一个下载任务，每个合约下载完整存续期。最多同时运行 2 个任务，每个任务使用一半的请求上限。",
                )}
              </p>
            )}
            {submitted !== null && (
              <p role="status">
                {t("已提交 {done} / {total} 个下载任务", {
                  done: submitted,
                  total: targets.length,
                })}
              </p>
            )}
            <button className="primary" type="submit" disabled={!targets.length}>
              {t(
                pending ? "确认下载提交" : all ? "下载全部 {n} 个合约" : "下载整个合约",
                all ? { n: targets.length } : undefined,
              )}
            </button>
          </fieldset>
        </form>
        <details className="history-notes">
          <summary>{t("数据说明")}</summary>
          <p className="subtle">
            {t(
              source.timeAxis === "trading-day"
                ? "日线保存供应商交易日期与结算价，可作为研究和模拟交易的 K 线或结算价来源；不补造缺失的交易日。"
                : "分钟数据按 K 线结束时间保存，交易日来自交易所交易日历，可用于研究和模拟交易。不补造缺失的 K 线或主力连续合约。",
            )}
          </p>
        </details>
      </section>
      <section className="history-activity" aria-label={t("下载记录")}>
        {error && (
          <p role="alert">
            <ErrorNotice error={error} namespace="asterion.terminal.data-workbench" />
          </p>
        )}
        <h3>{t("下载任务")}</h3>
        {!tasks.some(task => task.state !== "succeeded") && (
          <p className="history-empty">{t("暂无进行中的下载")}</p>
        )}
        {tasks.length > 0 && (
          <div
            className="publication-list"
            aria-label={t(source.timeAxis === "trading-day" ? "日线下载任务" : "分钟下载任务")}
          >
            {[...tasks]
              .reverse()
              .filter(task => task.state !== "succeeded")
              .map(task => (
                <article className="publication-row" data-task-id={task.id} key={task.id}>
                  <div className="publication-name">
                    <strong>{task.source_name}</strong>
                    <small>
                      {t(states[task.state])}
                      {!research?.online && ` · ${t("最后确认状态")}`} · {task.completed}/
                      {task.total} {t("时间分段")}
                    </small>
                    {task.error && (
                      <ErrorNotice error={new BackendError("operation_failed", task.error)} />
                    )}
                  </div>
                  <div>
                    {["queued", "running"].includes(task.state) && (
                      <button
                        disabled={busy || !research?.online}
                        onClick={() =>
                          void run("research.action", { id: task.id, action: "cancel" })
                        }
                      >
                        {t("取消")}
                      </button>
                    )}
                    {["failed", "cancelled", "interrupted"].includes(task.state) && (
                      <button
                        disabled={busy || !research?.online}
                        onClick={() =>
                          void run("research.action", { id: task.id, action: "retry" })
                        }
                      >
                        {t("继续下载")}
                      </button>
                    )}
                  </div>
                </article>
              ))}
          </div>
        )}
        <h3 className="history-datasets-title">{t("已下载数据集")}</h3>
        <p className="subtle">
          {t("下载完成不代表交易时段完整；数据保留原始来源，不混合不同数据商。")}
        </p>
        {!tasks.some(task => task.state === "succeeded") && (
          <p className="history-empty">{t("完成的下载将在这里显示")}</p>
        )}
        {tasks
          .filter(task => task.state === "succeeded")
          .reverse()
          .map(task => (
            <article className="publication-row" data-task-id={task.id} key={task.id}>
              <div className="publication-name">
                <strong>{task.source_name}</strong>
                <small>
                  {source.name} · {t(source.dataType)}
                </small>
              </div>
              <button
                disabled={busy || !research?.online}
                onClick={() => {
                  setToken("");
                  setViewId(task.id);
                }}
              >
                {t("查看数据")}
              </button>
              <button
                className="primary"
                disabled={busy || !research?.online || !task.history_dataset_id}
                onClick={() => {
                  setToken("");
                  navigate("workspace.research", {
                    page: "backtest",
                    params: {
                      source_dataset_id: task.history_dataset_id!,
                      connection_id: research?.connection_id ?? "",
                      selection_id: crypto.randomUUID(),
                    },
                  });
                }}
              >
                {t("用于回测")}
              </button>
              {task.kind === "daily_download" && (
                <button
                  disabled={busy || !research?.online || !task.history_dataset_id}
                  onClick={() => {
                    setToken("");
                    navigate("workspace.research", {
                      page: "daily_factor",
                      params: {
                        source_dataset_id: task.history_dataset_id!,
                        connection_id: research?.connection_id ?? "",
                      },
                    });
                  }}
                >
                  {t("日线因子分析")}
                </button>
              )}
              <button
                disabled={busy || !research?.online}
                onClick={() => void run("research.result", { id: task.id })}
              >
                {t("数据集详情")}
              </button>
            </article>
          ))}
        {result && (
          <details open className="futures-help">
            <summary>{t(source.timeAxis === "trading-day" ? "日线数据集" : "分钟数据集")}</summary>
            <p>
              {result.contract} · {result.interval === "day" ? t("日K") : `${result.interval} min`}{" "}
              · {result.rows} {t("根 K 线")}
            </p>
            <p>
              {source.name} · {source.timezone}
            </p>
            <p>
              {t("请求范围")} ·{" "}
              {source.timeAxis === "trading-day" ? result.begin : timestamp(result.begin)} —{" "}
              {source.timeAxis === "trading-day" ? result.end : timestamp(result.end)}
            </p>
            <p>
              {t("质量状态")} · {t("结构与摘要已校验；交易时段覆盖未校验")}
            </p>
            <details>
              <summary>{t("存储与校验")}</summary>
              <p>
                {t("数据目录")} <code>{result.directory}</code>
              </p>
              <p>
                SHA-256 <code>{result.digest}</code>
              </p>
            </details>
            {result.rows === 0 && <p>{t("请求区间内没有返回数据，请核对合约和时间范围。")}</p>}
          </details>
        )}
      </section>
    </div>
  );
}

export function HistoricalDownloads(context: TerminalContext) {
  const sources = historySources(context.snapshot?.research?.sources ?? []);
  const [sourceId, setSourceId] = useWorkspaceDraft("historySource", "");
  const source = sources.find(item => item.id === sourceId) ?? sources[0];
  return (
    <section className="futures-data history-page" aria-label={t("历史数据")}>
      <header className="history-heading">
        <div>
          <span className="history-eyebrow">DATA / HISTORY</span>
          <h2>{t("历史数据")}</h2>
          <p className="subtle">{t("为研究积累可追溯的合约历史数据")}</p>
        </div>
        <label>
          {t("数据源")}
          <select
            aria-label={t("数据源")}
            value={source?.id ?? ""}
            onChange={event => setSourceId(event.target.value)}
          >
            {sources.map(item => (
              <option key={item.id} value={item.id}>
                {item.name} · {t(item.dataType)}
              </option>
            ))}
          </select>
        </label>
      </header>
      {source ? (
        <>
          <div className="history-capabilities">
            <span>{t(source.asset)}</span>
            <span>{t(source.dataType)}</span>
            <span>
              {source.intervals
                .map(value => (value === "day" ? t("日K") : `${value} min`))
                .join(" / ")}
            </span>
            <span>{source.timezone}</span>
          </div>
          <SourceDownloads
            key={JSON.stringify([source.id, context.snapshot?.research?.connection_id])}
            {...context}
            source={source}
          />
        </>
      ) : (
        <>{context.snapshot?.research?.online && <p role="alert">{t("数据源不可用")}</p>}</>
      )}
    </section>
  );
}
