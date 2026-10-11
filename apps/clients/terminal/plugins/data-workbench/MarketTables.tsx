import "./data.css";
import { useCallback, useEffect, useRef, useState } from "react";
import {
  type TerminalContext,
  translate,
  type MessageValues,
  BackendError,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  getLocale,
} from "../contract";
import type {
  Snapshot,
  TableCoverage,
  TableCoverageRow,
  TablePage,
  TableSource,
} from "../../src/bridge/client";
import { tableNames } from "./table-names";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);

// How each table's columns are shown: a label, and whether the cells are
// numbers, which line up on the right like every number in the terminal. A
// name means what its table says: a volume is contracts traded in one and
// receipts registered in another. Tables and columns this page does not know
// show as stored.
type Shown = readonly [label: string, numeric?: true];
const shown: Record<string, Record<string, Shown>> = {
  "futures.contracts": {
    contract: ["合约"],
    source_instrument: ["数据源代码"],
    name: ["名称"],
    list_date: ["上市日"],
    delist_date: ["最后交易日"],
    last_delivery_date: ["最后交割日"],
    multiplier: ["合约乘数", true],
    per_unit: ["每手数量", true],
    trade_unit: ["交易单位"],
    quote_unit: ["报价单位"],
    tick_description: ["最小变动价位"],
    delivery_description: ["交割方式"],
    trading_hours: ["交易时间"],
  },
  "futures.calendar": {
    day: ["日期"],
    open: ["开市"],
    previous_trading_day: ["上一交易日"],
  },
  "futures.daily": {
    contract: ["合约"],
    open: ["开盘价", true],
    high: ["最高价", true],
    low: ["最低价", true],
    close: ["收盘价", true],
    settlement: ["结算价", true],
    previous_close: ["昨收", true],
    previous_settlement: ["昨结算", true],
    volume: ["成交量", true],
    amount: ["成交额", true],
    open_interest: ["持仓量", true],
    open_interest_change: ["持仓量变化", true],
    delivery_settlement: ["交割结算价", true],
  },
  "futures.settlement": {
    contract: ["合约"],
    trading_fee_rate: ["交易手续费率", true],
    trading_fee: ["交易手续费", true],
    close_today_fee: ["平今手续费", true],
    delivery_fee: ["交割手续费", true],
    long_margin_rate: ["买投机保证金率", true],
    short_margin_rate: ["卖投机保证金率", true],
    buy_hedging_margin_rate: ["买套保保证金率", true],
    sell_hedging_margin_rate: ["卖套保保证金率", true],
  },
  "futures.receipts": {
    product: ["品种"],
    line: ["行", true],
    product_name: ["品种名称"],
    warehouse: ["仓库"],
    warehouse_id: ["仓库编号"],
    area: ["地区"],
    year: ["年度"],
    grade: ["等级"],
    brand: ["品牌"],
    place: ["产地"],
    converted: ["折算仓单"],
    previous_volume: ["昨日仓单量", true],
    volume: ["今日仓单量", true],
    change: ["增减", true],
    unit: ["单位"],
    premium: ["升贴水", true],
  },
  "futures.rankings": {
    instrument: ["合约或品种"],
    member: ["会员"],
    volume: ["成交量", true],
    volume_change: ["成交量变化", true],
    long: ["持买仓量", true],
    long_change: ["持买仓量变化", true],
    short: ["持卖仓量", true],
    short_change: ["持卖仓量变化", true],
  },
};
// What a reader has to know before using a table's numbers.
const notes: Record<string, string> = {
  "futures.settlement": "结算参数说明",
  "futures.receipts": "仓单日报说明",
  "futures.rankings": "成交持仓排名说明",
};
const named = (names: Record<string, string>, key: string) => (names[key] ? t(names[key]) : key);
const columnName = (table: string, column: string) => {
  const known = shown[table]?.[column];
  return known ? t(known[0]) : column;
};
const numeric = (table: string, column: string) =>
  shown[table]?.[column]?.[1] ? "numeric" : undefined;
// Which tables are kept by trading day: the ones an update is asked for.
const byDay = (table: string) => table !== "futures.contracts" && table !== "futures.calendar";
function yesterday() {
  return new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Shanghai",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).format(new Date(Date.now() - 86400000));
}
function acquired(ns: string) {
  return new Intl.DateTimeFormat(getLocale(), { dateStyle: "short", timeStyle: "short" }).format(
    new Date(Number(ns.slice(0, -6))),
  );
}
const savedFor = (snapshot: Snapshot | null, provider: string) => {
  const entry = snapshot?.data_credentials?.find(item => item.provider === provider);
  return entry && !("error" in entry) && entry.credential_ready ? entry : undefined;
};

export function MarketTables(context: TerminalContext) {
  const [coverage, setCoverage] = useState<TableCoverage | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const [opened, setOpened] = useState<{ source: TableSource; exchange: string } | null>(null);
  const query = useRef(context.query);
  query.current = context.query;
  const online = !!context.snapshot?.data?.online;
  const connection = context.snapshot?.data?.connection_id;
  // Replies of an earlier request, or to an earlier connection, are dropped.
  const sequence = useRef({ value: 0 });
  const load = useCallback(async () => {
    const generation = sequence.current;
    const current = ++generation.value;
    setLoading(true);
    setError("");
    try {
      const response = await query.current("data.tables.coverage", {});
      if (current === generation.value) setCoverage(response.table_coverage ?? null);
    } catch (e) {
      if (current === generation.value) setError(asDisplayError(e));
    } finally {
      if (current === generation.value) setLoading(false);
    }
  }, []);
  useEffect(() => {
    const generation = sequence.current;
    setCoverage(null);
    setOpened(null);
    if (online) void load();
    return () => {
      ++generation.value;
    };
  }, [online, connection, load]);
  // What an update stores appears as its tasks end.
  const finished = (context.snapshot?.task_service?.tasks ?? [])
    .filter(task => task.kind === "table_update" && task.state !== "queued")
    .map(task => `${task.id}:${task.state}:${task.completed}`)
    .join(" ");
  useEffect(() => {
    if (online && finished) void load();
  }, [online, finished, load]);
  const sources = coverage?.sources ?? [];
  const held = (source: TableSource, exchange: string) =>
    coverage?.rows.find(
      row =>
        row.table === source.table && row.source === source.source && row.exchange === exchange,
    );
  return (
    <section className="data-archive market-tables" aria-label={t("全市场数据")}>
      <h2>{t("全市场数据")}</h2>
      <p className="subtle">{t("全市场数据说明")}</p>
      {!online && <p role="status">{t("数据服务未连接")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {online && coverage && !sources.length && (
        <p role="status">{t("已安装的数据源都不提供全市场数据表。")}</p>
      )}
      {!!sources.length && (
        <>
          <TableUpdateForm context={context} sources={sources} coverage={coverage} />
          <div className="source-actions">
            <h3>{t("已入库的数据")}</h3>
            <button type="button" disabled={loading || context.busy} onClick={() => void load()}>
              {t("刷新")}
            </button>
          </div>
          <table className="coverage-table" aria-label={t("全市场数据覆盖")}>
            <thead>
              <tr>
                <th>{t("数据表")}</th>
                <th>{t("交易所")}</th>
                <th>{t("范围")}</th>
                <th className="numeric">{t("分区")}</th>
                <th className="numeric">{t("行数")}</th>
                <th>{t("缺少的交易日")}</th>
                <th>{t("最近取得")}</th>
                <th>{t("操作")}</th>
              </tr>
            </thead>
            <tbody>
              {sources.flatMap(source =>
                source.venues.map(exchange => {
                  const row = held(source, exchange);
                  return (
                    <tr key={`${source.table}:${source.source}:${exchange}`}>
                      <td>{named(tableNames, source.table)}</td>
                      <td>{exchange}</td>
                      {row ? (
                        <>
                          <td>{range(row)}</td>
                          <td className="numeric">{row.partitions}</td>
                          <td className="numeric">{row.rows}</td>
                          <td>{gaps(row)}</td>
                          <td>{acquired(row.acquired_at_ns)}</td>
                          <td>
                            <button
                              type="button"
                              aria-pressed={
                                opened?.source.table === source.table &&
                                opened.source.source === source.source &&
                                opened.exchange === exchange
                              }
                              onClick={() => setOpened({ source, exchange })}
                            >
                              {t("查看数据")}
                            </button>
                          </td>
                        </>
                      ) : (
                        <td colSpan={6} className="subtle">
                          {t("尚未下载")}
                        </td>
                      )}
                    </tr>
                  );
                }),
              )}
            </tbody>
          </table>
          {coverage?.snapshot && (
            <p className="subtle">
              {t("当前快照 {id}：引用它的任务读到的始终是此刻的数据。", {
                id: coverage.snapshot.slice(0, 12),
              })}
            </p>
          )}
        </>
      )}
      {opened && held(opened.source, opened.exchange) && (
        <PartitionViewer
          key={`${opened.source.table}:${opened.source.source}:${opened.exchange}`}
          context={context}
          row={held(opened.source, opened.exchange)!}
          onClose={() => setOpened(null)}
        />
      )}
    </section>
  );
}
function range(row: TableCoverageRow) {
  if (row.first_period === "all") return t("全部");
  return row.first_period === row.last_period
    ? row.first_period
    : `${row.first_period} — ${row.last_period}`;
}
function gaps(row: TableCoverageRow) {
  if (row.missing_days === null) return "—";
  if (!row.missing_days) return t("无");
  return t("{count} 个：{days}", {
    count: row.missing_days,
    days:
      row.missing_examples.join("、") +
      (row.missing_days > row.missing_examples.length ? " …" : ""),
  });
}

// Asks for the day tables of one data source. What is read is decided when the
// task runs: the trading days the store does not hold yet.
function TableUpdateForm({
  context,
  sources,
  coverage,
}: {
  context: TerminalContext;
  sources: TableSource[];
  coverage: TableCoverage | null;
}) {
  const providers = [...new Set(sources.map(source => source.plugin_id))];
  const [provider, setProvider] = useState(providers[0]);
  const offered = sources.filter(source => source.plugin_id === provider && byDay(source.table));
  const [chosen, setChosen] = useState<string[] | null>(null);
  const tables = (chosen ?? offered.map(source => source.table)).filter(table =>
    offered.some(source => source.table === table),
  );
  const [begin, setBegin] = useState("");
  const [end, setEnd] = useState("");
  const [token, setToken] = useState("");
  // The identity of the last request and what it asked for.
  const [request, setRequest] = useState({ id: "", definition: "", accepted: false });
  const id = request.id;
  const [working, setWorking] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const snapshot = context.snapshot;
  // The provider's connection rules come with any one of its history sources.
  const connection = snapshot?.data?.sources.find(source => source.plugin_id === provider);
  const saved = savedFor(snapshot, provider);
  const [rate, setRate] = useState(
    String(connection?.connection?.requests_per_minute_default ?? 60),
  );
  const requests = Number(saved?.requests_per_minute ?? rate);
  // With nothing stored there is no day to continue from. Once some exchange
  // holds data an update may continue; the others are named in its result.
  const empty = !offered.some(
    source =>
      tables.includes(source.table) &&
      coverage?.rows.some(row => row.table === source.table && row.source === source.source),
  );
  // This page's own request, or else the latest update: one started before
  // the page was opened is still worth watching.
  const updates = (snapshot?.task_service?.tasks ?? []).filter(
    item => item.kind === "table_update",
  );
  const task = id
    ? updates.find(item => item.id === id)
    : updates.reduce<(typeof updates)[number] | undefined>(
        (latest, item) =>
          latest && latest.submission_sequence > item.submission_sequence ? latest : item,
        undefined,
      );
  const shown = task?.id ?? "";
  const result =
    shown && snapshot?.task_result?.id === shown && snapshot.task_result.kind === "table_update"
      ? snapshot.task_result.result
      : null;
  const running = !!task && ["queued", "running", "cancel_requested"].includes(task.state);
  const enabled = !!snapshot?.task_service?.online && !!snapshot?.data?.online;
  const succeeded = task?.state === "succeeded";
  const read = useRef(context.trade);
  read.current = context.trade;
  useEffect(() => {
    if (succeeded && shown)
      void read.current("task.result", { id: shown }).catch(e => setError(asDisplayError(e)));
  }, [succeeded, shown]);
  async function act(action: () => Promise<void>) {
    setWorking(true);
    setError("");
    try {
      await action();
    } catch (e) {
      setError(asDisplayError(e));
    } finally {
      setWorking(false);
    }
  }
  const configureBudget = () =>
    act(() =>
      context.trade("data.download.budget.configure", {
        source: connection?.id ?? "",
        token: saved ? "" : token,
        requests_per_minute: requests,
      }),
    );
  // The same request keeps its identity until it is accepted: asking again
  // after a lost reply confirms the first task instead of creating another.
  const submit = () =>
    act(async () => {
      const definition = {
        provider,
        tables: [...tables].sort(),
        begin_day: begin,
        end_day: end,
        requests_per_minute: requests,
      };
      const key = JSON.stringify(definition);
      const next =
        request.id && !request.accepted && request.definition === key
          ? request.id
          : `tables-${crypto.randomUUID()}`;
      setRequest({ id: next, definition: key, accepted: false });
      try {
        await context.trade("data.tables.update.submit", {
          id: next,
          ...definition,
          token: saved ? "" : token,
        });
        setRequest({ id: next, definition: key, accepted: true });
      } finally {
        setToken("");
      }
    });
  return (
    <section className="history-update" aria-label={t("更新全市场数据")}>
      <h3>{t("更新全市场数据")}</h3>
      <form
        onSubmit={event => {
          event.preventDefault();
          void submit();
        }}
      >
        <fieldset disabled={!enabled || working || context.busy || running}>
          <div className="futures-fields">
            {providers.length > 1 && (
              <label>
                {t("数据源")}
                <select value={provider} onChange={event => setProvider(event.target.value)}>
                  {providers.map(item => (
                    <option key={item} value={item}>
                      {item}
                    </option>
                  ))}
                </select>
              </label>
            )}
            <label>
              {t("起始日期")}
              <input
                type="date"
                required={empty}
                max={end || yesterday()}
                value={begin}
                onChange={event => setBegin(event.target.value)}
              />
            </label>
            <label>
              {t("结束日期")}
              <input
                type="date"
                min={begin || undefined}
                max={yesterday()}
                value={end}
                onChange={event => setEnd(event.target.value)}
              />
            </label>
            {!saved && (
              <label>
                {t("每分钟请求数")}
                <input
                  required
                  type="number"
                  min="1"
                  max={connection?.connection?.requests_per_minute_max ?? 500}
                  value={rate}
                  onChange={event => setRate(event.target.value)}
                />
              </label>
            )}
            {!saved && connection?.credential_required && (
              <label>
                {connection.connection
                  ? getLocale() === "zh-CN"
                    ? connection.connection.credential_label_zh
                    : connection.connection.credential_label_en
                  : t("数据源凭据")}
                <input
                  type="password"
                  autoComplete="off"
                  maxLength={connection.connection?.credential_max_length ?? 256}
                  value={token}
                  onChange={event => setToken(event.target.value)}
                />
              </label>
            )}
          </div>
          {offered.length > 1 && (
            <div className="source-actions" role="group" aria-label={t("数据表")}>
              {offered.map(source => (
                <label key={source.table}>
                  <input
                    type="checkbox"
                    checked={tables.includes(source.table)}
                    onChange={event =>
                      setChosen(
                        event.target.checked
                          ? [...tables, source.table]
                          : tables.filter(table => table !== source.table),
                      )
                    }
                  />
                  {named(tableNames, source.table)}
                </label>
              ))}
            </div>
          )}
          <p className="subtle">
            {t(
              empty
                ? "还没有数据，第一次下载需要起始日期。结束日期留空则取到上海时间已经结束的最后一天。"
                : "起始日期留空则接着已有数据往后取；填写后会补齐这一天起缺少的交易日。结束日期留空则取到上海时间已经结束的最后一天。",
            )}
          </p>
          <p className="subtle">
            {t("同一账号的下载共享当前数据服务的额度；多节点请分别分配份额。")}
          </p>
          <div className="source-actions">
            <button
              type="button"
              disabled={!connection || (!saved && !!connection.credential_required && !token)}
              onClick={() => void configureBudget()}
            >
              {t("应用共享额度")}
            </button>
            <button
              type="submit"
              className="primary"
              disabled={!tables.length || (!saved && !!connection?.credential_required && !token)}
            >
              {t("开始更新")}
            </button>
          </div>
        </fieldset>
      </form>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {task && (
        <div className="history-update-preview" aria-live="polite">
          {running && (
            <p role="status">
              {t(task.state === "queued" ? "更新任务排队中" : "正在更新：{percent}%", {
                percent: task.completed,
              })}
            </p>
          )}
          {running && (
            <button
              type="button"
              disabled={context.busy || task.state === "cancel_requested"}
              onClick={() =>
                void act(() => context.trade("task.action", { id: shown, action: "cancel" }))
              }
            >
              {t("停止更新")}
            </button>
          )}
          {["failed", "interrupted", "cancelled"].includes(task.state) && (
            <>
              <p role="alert">
                {t(
                  task.state === "cancelled"
                    ? "更新已停止。已入库的数据保留，再次更新只取剩下的。"
                    : "更新没有完成。已入库的数据保留，再次更新只取剩下的。",
                )}
              </p>
              {task.error && (
                <p role="alert">
                  <ErrorNotice error={new BackendError("operation_failed", task.error)} />
                </p>
              )}
            </>
          )}
          {succeeded && result && (
            <>
              <p role="status">
                {t("更新完成：新入库 {stored} 个分区，{unchanged} 个没有变化。", {
                  stored: result.stored,
                  unchanged: result.unchanged,
                })}
              </p>
              {!!result.unpublished && (
                <p>
                  {t(
                    "数据源对 {count} 个分区没有返回数据，例如 {examples}。可能尚未发布，也可能数据源没有这段历史；下次更新会再取。",
                    {
                      count: result.unpublished,
                      examples: result.unpublished_examples
                        .slice(0, 3)
                        .map(
                          item =>
                            `${named(tableNames, item.table)} ${item.exchange} ${item.period}`,
                        )
                        .join("、"),
                    },
                  )}
                </p>
              )}
              {!!result.waiting.length && (
                <p>
                  {t(
                    "有 {count} 组数据表和交易所还没有数据，需要给出起始日期才会下载，例如 {examples}。",
                    {
                      count: result.waiting.length,
                      // Each is a table and an exchange, as the plan names them.
                      examples: result.waiting
                        .slice(0, 3)
                        .map(item => {
                          const [table, exchange] = item.split(" ");
                          return `${named(tableNames, table)} ${exchange}`;
                        })
                        .join("、"),
                    },
                  )}
                </p>
              )}
              {!!result.rejected.length && (
                <table className="coverage-table" aria-label={t("未能入库的分区")}>
                  <thead>
                    <tr>
                      <th>{t("未能入库的分区")}</th>
                      <th>{t("原因")}</th>
                    </tr>
                  </thead>
                  <tbody>
                    {result.rejected.map(item => (
                      <tr key={`${item.table}:${item.exchange}:${item.period}`}>
                        <td>
                          {named(tableNames, item.table)} {item.exchange} {item.period}
                        </td>
                        <td>
                          <code>{item.reason}</code>
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </>
          )}
        </div>
      )}
    </section>
  );
}

// One stored partition, a page at a time, exactly as it is kept.
function PartitionViewer({
  context,
  row,
  onClose,
}: {
  context: TerminalContext;
  row: TableCoverageRow;
  onClose: () => void;
}) {
  const limit = 100;
  const [period, setPeriod] = useState(row.last_period);
  const [page, setPage] = useState<TablePage | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [loading, setLoading] = useState(false);
  const query = useRef(context.query);
  query.current = context.query;
  const sequence = useRef({ value: 0 });
  const panel = useRef<HTMLElement>(null);
  useEffect(() => {
    panel.current?.scrollIntoView({ block: "nearest" });
  }, []);
  const fetchPage = useCallback(
    async (target: string, offset: number) => {
      const generation = sequence.current;
      const current = ++generation.value;
      setLoading(true);
      setError("");
      try {
        const response = await query.current("data.tables.page", {
          table: row.table,
          source: row.source,
          exchange: row.exchange,
          period: target,
          offset,
          limit,
        });
        if (current === generation.value) setPage(response.table_page ?? null);
      } catch (e) {
        if (current === generation.value) {
          setPage(null);
          setError(asDisplayError(e));
        }
      } finally {
        if (current === generation.value) setLoading(false);
      }
    },
    [row.table, row.source, row.exchange],
  );
  useEffect(() => {
    const generation = sequence.current;
    void fetchPage(row.last_period, 0);
    return () => {
      ++generation.value;
    };
  }, [fetchPage, row.last_period]);
  const kind = row.first_period === "all" ? "all" : row.first_period.length === 4 ? "year" : "day";
  return (
    <section
      ref={panel}
      className="history-update"
      aria-label={t("分区数据")}
      data-table={row.table}
    >
      <div className="source-actions">
        <h3>
          {named(tableNames, row.table)} · {row.exchange}
        </h3>
        <button type="button" onClick={onClose}>
          {t("关闭")}
        </button>
      </div>
      {kind !== "all" && (
        <form
          className="source-actions"
          onSubmit={event => {
            event.preventDefault();
            void fetchPage(period, 0);
          }}
        >
          <label>
            {t(kind === "year" ? "年份" : "交易日")}
            <input
              required
              type={kind === "year" ? "number" : "date"}
              min={row.first_period}
              max={row.last_period}
              value={period}
              onChange={event => setPeriod(event.target.value)}
            />
          </label>
          <button disabled={loading}>{t("查看")}</button>
        </form>
      )}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
      {notes[row.table] && <p className="subtle">{t(notes[row.table])}</p>}
      {page && (
        <>
          <p className="subtle">
            {t("{period} · {rows} 行 · 取得于 {time} · 内容摘要 {content}", {
              period: page.revision.period === "all" ? t("全部") : page.revision.period,
              rows: page.revision.rows,
              time: acquired(page.revision.acquired_at_ns),
              content: page.revision.content.slice(0, 12),
            })}
          </p>
          <div className="history-usage-table">
            <table className="coverage-table" aria-label={t("分区数据")}>
              <thead>
                <tr>
                  {page.columns.map(column => (
                    <th key={column} className={numeric(row.table, column)}>
                      {columnName(row.table, column)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {page.rows.map((cells, index) => (
                  <tr key={page.offset + index}>
                    {cells.map((cell, column) => (
                      <td
                        key={page.columns[column]}
                        className={numeric(row.table, page.columns[column])}
                      >
                        {cell ?? "—"}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="source-actions">
            <button
              type="button"
              disabled={loading || !page.offset}
              onClick={() => void fetchPage(page.revision.period, Math.max(0, page.offset - limit))}
            >
              {t("上一页")}
            </button>
            <span className="subtle">
              {t("第 {first}–{last} 行，共 {total} 行", {
                first: page.offset + 1,
                last: page.offset + page.rows.length,
                total: page.revision.rows,
              })}
            </span>
            <button
              type="button"
              disabled={loading || page.offset + page.rows.length >= page.revision.rows}
              onClick={() => void fetchPage(page.revision.period, page.offset + limit)}
            >
              {t("下一页")}
            </button>
          </div>
        </>
      )}
    </section>
  );
}
