import { HistoryDatasetViewer } from "./HistoryDatasetViewer";
import { useState } from "react";
import { historySources, type HistorySource } from "./history-sources";
import { timestamp } from "../../src/bridge/client";
import { BackendError } from "../../src/i18n/errors";
import {
  type TerminalContext,
  useWorkspaceDraft,
  useWorkspaceRequestId,
  translate,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
} from "../contract";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
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
  source,
}: Pick<TerminalContext, "snapshot" | "busy" | "trade" | "query" | "navigate"> & {
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
  const [token, setToken] = useState(""); // Never retain credentials in workspace drafts/storage.
  const [error, setError] = useState<DisplayError>("");
  const research = snapshot?.research;
  const catalog = source.catalog(snapshot);
  const matchingCatalog =
    catalog?.exchange === form.exchange && catalog.product === form.product ? catalog : null;
  const contracts = matchingCatalog?.items ?? [];
  const selected = contracts.find(item => item.code === form.code);
  const cutoff = matchingCatalog?.cutoff_ns ?? "0";
  const [pending, setPending] = useWorkspaceRequestId(
    `historySubmission:${source.id}`,
    JSON.stringify([research?.connection_id, form, cutoff]),
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
        <p className="dashboard-caption">{t(source.description)}</p>
        {research?.remote ? <p>{t("历史下载当前使用本机研究服务。")}</p> : null}
        {(!research?.online || research.remote) && (
          <button disabled={busy} onClick={() => void run("research.local")}>
            {t("连接本机研究服务")}
          </button>
        )}
        <form
          onSubmit={event => {
            event.preventDefault();
            if (!selected || !matchingCatalog) return;
            const id = pending ?? `history-${crypto.randomUUID()}`;
            setPending(id);
            void run(source.command, { id, ...source.parameters(form, token, matchingCatalog) })
              .then(ok => {
                if (ok) setPending(null);
              })
              .finally(() => setToken(""));
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
                {t(source.credential.label)}
                <input
                  type="password"
                  autoComplete="off"
                  required={source.credential.required}
                  maxLength={source.credential.maxLength}
                  value={token}
                  onChange={event => setToken(event.target.value)}
                />
              </label>
              <div className="history-catalog-action">
                <button
                  type="button"
                  disabled={!token || !form.product}
                  onClick={() =>
                    void run(source.catalogCommand, {
                      exchange: form.exchange,
                      product: form.product,
                      token,
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
                  value={selected ? form.code : ""}
                  onChange={event => update("code", event.target.value)}
                >
                  <option value="">{t("选择具体月份合约")}</option>
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
                  value={form.rate}
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
            <button className="primary" type="submit" disabled={!selected}>
              {t(pending ? "确认下载提交" : "下载整个合约")}
            </button>
          </fieldset>
        </form>
        <details className="history-notes">
          <summary>{t("数据说明")}</summary>
          <p className="subtle">
            {t(
              source.timeAxis === "trading-day"
                ? "日线独立保存供应商交易日期与可缺失结算价；不补造交易日期或逐笔成交，不直接用于逐笔回测。"
                : "分钟数据独立保存，保留供应商时间标签；不补造交易日、主力连续合约或逐笔成交。现有逐笔回测不直接使用分钟数据。",
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
                <article className="publication-row" key={task.id}>
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
            <article className="publication-row" key={task.id}>
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
              {task.kind === "daily_download" && (
                <button
                  disabled={busy || !research?.online}
                  onClick={() => {
                    setToken("");
                    navigate("workspace.research", {
                      page: "daily_factor",
                      params: {
                        source_task_id: task.id,
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
  const [sourceId, setSourceId] = useWorkspaceDraft("historySource", historySources[0].id);
  const source = historySources.find(item => item.id === sourceId);
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
            value={sourceId}
            onChange={event => setSourceId(event.target.value)}
          >
            {historySources.map(item => (
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
        <p role="alert">{t("数据源不可用")}</p>
      )}
    </section>
  );
}
