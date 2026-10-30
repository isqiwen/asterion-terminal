import { useEffect, useRef, useState, type ReactNode } from "react";
import {
  timestamp,
  type HistoryDatasetRecord,
  type IntradaySeries,
  type LiveMarket,
} from "../../src/bridge/client";
import {
  translate,
  useWorkspaceDraft,
  ErrorNotice,
  asDisplayError,
  type DisplayError,
  type TerminalContext,
  type MessageValues,
} from "../contract";
import { ContractHistory } from "./ContractHistory";
import { observedRuns, labelBoundary } from "./minute-boundary";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);
const ns = (ms: number) => String(BigInt(ms) * 1000000n);
export function MinuteContinuity({
  context,
  market,
  row,
  series,
  error,
  renderLive,
  pane,
}: {
  pane: string;
  context: TerminalContext;
  market: LiveMarket;
  row: LiveMarket["subscriptions"][number];
  series: IntradaySeries | null;
  error: DisplayError;
  renderLive: (series: IntradaySeries) => ReactNode;
}) {
  const contract = market.catalog.contracts.find(
    c => c.venue === row.venue && c.symbol === row.symbol,
  )?.contract_id;
  const online = !!context.snapshot?.data?.online;
  const connection = context.snapshot?.data?.connection_id;
  const dataKey = JSON.stringify([connection, online, contract]);
  const query = useRef(context.query);
  query.current = context.query;
  const [catalog, setCatalog] = useState<{
    key: string;
    items: HistoryDatasetRecord[];
    error: DisplayError;
  }>();
  useEffect(() => {
    if (!online || !contract) return;
    let active = true;
    void query
      .current("data.datasets", {
        venue: "",
        product: "",
        contract_id: contract,
        source: "tushare.ft_mins",
      })
      .then(reply => {
        if (active)
          setCatalog({
            key: dataKey,
            items: (reply.history_datasets ?? []).filter(
              v =>
                v.contract_id === contract &&
                v.source === "tushare.ft_mins" &&
                v.interval_minutes === 1,
            ),
            error: "",
          });
      })
      .catch(reason => {
        if (active) setCatalog({ key: dataKey, items: [], error: asDisplayError(reason) });
      });
    return () => {
      active = false;
    };
  }, [dataKey, online, contract]);
  const items = catalog?.key === dataKey ? catalog.items : [];
  const [version, setVersion] = useWorkspaceDraft<{ key: string; id: string }>(
    `${pane}.boundary-version`,
    { key: "", id: "" },
  );
  const chosen =
    version.key === dataKey && version.id ? items.find(v => v.id === version.id) : items[0];
  const tailKey = JSON.stringify([dataKey, chosen?.id, chosen?.revision, chosen?.rows]);
  const [tail, setTail] = useState<{
    key: string;
    last: string | null;
    manifest: string;
    error: DisplayError;
  }>();
  useEffect(() => {
    if (!online || !chosen?.rows) return;
    let active = true;
    void query
      .current("data.minutes.page", {
        include_macd: false,
        id: chosen.id,
        offset: chosen.rows - 1,
        limit: 1,
        start: "",
        end: "",
      })
      .then(reply => {
        if (!active) return;
        const page = reply.history_page;
        if (
          !page ||
          page.id !== chosen.id ||
          page.contract_id !== contract ||
          page.source !== chosen.source ||
          page.interval_minutes !== 1 ||
          page.manifest_sha256 !== chosen.revision ||
          page.total_rows !== chosen.rows ||
          page.offset !== chosen.rows - 1 ||
          page.bars.length !== 1
        )
          throw new Error(t("返回的数据与所选固定分钟档案不匹配"));
        setTail({
          key: tailKey,
          last: page.bars.at(-1)?.timestamp_ns ?? null,
          manifest: page.manifest_sha256,
          error: "",
        });
      })
      .catch(reason => {
        if (active)
          setTail({ key: tailKey, last: null, manifest: "", error: asDisplayError(reason) });
      });
    return () => {
      active = false;
    };
  }, [tailKey, online, chosen, contract]);
  const currentTail = tail?.key === tailKey ? tail : undefined;
  const valid =
    series &&
    market.transport_online &&
    market.phase === "connected" &&
    series.service === market.service &&
    series.instance_id === market.instance_id &&
    series.venue === row.venue &&
    series.symbol === row.symbol
      ? series
      : null;
  const split = observedRuns(valid);
  const boundary = labelBoundary(currentTail?.last ?? null, valid);
  const viewKey = JSON.stringify([
    market.service,
    market.instance_id,
    row.venue,
    row.symbol,
    connection,
    contract,
  ]);
  const [view, setView] = useWorkspaceDraft<{ key: string; value: "live" | "archive" }>(
    `${pane}.minute-boundary-view`,
    { key: "", value: "live" },
  );
  const activeView = view.key === viewKey ? view.value : "live";
  const runKey = JSON.stringify([viewKey, valid?.trading_day, valid?.first_observation_ms]);
  const [run, setRun] = useWorkspaceDraft<{ key: string; start: number }>(
    `${pane}.minute-boundary-run`,
    { key: "", start: 0 },
  );
  const runs = split?.runs ?? [];
  const selectedRun =
    (run.key === runKey ? runs.find(r => r[0].start_ms === run.start) : undefined) ?? runs.at(-1);
  return (
    <section className="minute-continuity" aria-label={t("1分钟分段浏览")}>
      <div className="market-periods" role="group" aria-label={t("1分钟来源段")}>
        <button
          aria-pressed={activeView === "live"}
          onClick={() => setView({ key: viewKey, value: "live" })}
        >
          {t("本次观测（临时）")}
        </button>
        <button
          aria-pressed={activeView === "archive"}
          disabled={!chosen || !online}
          onClick={() => setView({ key: viewKey, value: "archive" })}
        >
          {t("已存1分档案（固定）")}
        </button>
      </div>
      <label>
        {t("对照的1分档案")}{" "}
        <select
          aria-label={t("对照的1分档案")}
          value={chosen?.id ?? ""}
          disabled={!items.length}
          onChange={e => setVersion({ key: dataKey, id: e.target.value })}
        >
          {!chosen && <option value="">{t("请选择可用固定版本")}</option>}
          {items.map(v => (
            <option key={v.id} value={v.id}>
              {v.id.slice(0, 12)} · {v.begin} — {v.end}
            </option>
          ))}
        </select>
      </label>
      {!contract && <p role="status">{t("当前目录没有固定历史合约身份；不猜测月份映射。")}</p>}
      {online && contract && catalog?.key === dataKey && !items.length && (
        <p role="status">{t("没有已存1分档案；不会自动下载。")}</p>
      )}
      {((catalog?.key === dataKey && catalog.error) || currentTail?.error) && (
        <p role="alert">
          <ErrorNotice
            error={(catalog?.key === dataKey && catalog.error) || currentTail?.error || ""}
          />
        </p>
      )}
      <p className="subtle">
        {t(
          "两段不自动拼接：旧档案接口未给出该版本标签语义，实时首分钟可能不完整；成交量口径不同。同一时段只画所选来源，不相加、不覆盖历史。",
        )}
      </p>
      <p role="status">
        {t(
          boundary.kind === "unknown"
            ? "边界未知：等待固定档案末根或本次观测。"
            : boundary.kind === "same_label"
              ? "边界标签相同：不等于同一根K线，仍分段。"
              : boundary.kind === "potential_overlap"
                ? "标签范围可能重叠：只显示所选来源，未作窗口等价去重。"
                : "两段存在标签间隔：不是缺失交易分钟判定，不补点。",
        )}
        {boundary.kind === "label_gap" &&
          boundary.gap_minutes !== null &&
          ` ${t("标签间隔 {minutes} 分钟", { minutes: boundary.gap_minutes })}`}
      </p>
      <details className="quote-data-note">
        <summary>{t("分钟边界与来源")}</summary>
        <p>
          {t("固定档案末根标签")} {currentTail?.last ? timestamp(currentTail.last) : "—"} ·
          Asia/Shanghai
        </p>
        <p>
          {t("档案版本摘要")} <code>{currentTail?.manifest ?? chosen?.revision ?? "—"}</code>
        </p>
        <p>
          {t("Data连接")} {connection ?? "—"} · {chosen?.source ?? "—"} · {contract ?? "—"}
        </p>
        <p>
          {t("临时行情服务")} {market.service} · {market.instance_id}
        </p>
        <p>
          {t("首条报价来源时刻")}{" "}
          {valid?.first_observation_ms ? timestamp(ns(valid.first_observation_ms)) : "—"} ·{" "}
          {valid?.trading_day ?? "—"}
        </p>
        <p>
          {t("保留观测起点标签")} {valid?.bars[0] ? timestamp(ns(valid.bars[0].start_ms)) : "—"}
        </p>
        <p>
          {t(
            "临时分钟用start_ms标记起点，最后一根未必结束；档案内部完整性与跨源同窗均未验证。不会写入档案或改变固定研究输入。",
          )}
        </p>
      </details>
      {activeView === "archive" ? (
        chosen && contract ? (
          <ContractHistory
            context={context}
            venue={row.venue}
            symbol={row.symbol}
            historyContractId={contract}
            fixedPeriod={1}
            fixedMinuteVersion={chosen.id}
            preferenceKey={`${pane}.minute-boundary-history`}
          />
        ) : (
          <p role="status">{t("请选择可用固定版本")}</p>
        )
      ) : (
        <>
          {error && (
            <p role="alert">
              <ErrorNotice error={error} />
            </p>
          )}
          {valid?.interrupted && (
            <p role="status">{t("本次观测曾中断，已有分钟也不能证明完整覆盖。")}</p>
          )}
          {valid && !split ? (
            <p role="alert">{t("观测分钟标签重复或乱序：拒绝绘制，不在界面猜测修复。")}</p>
          ) : selectedRun ? (
            <>
              <label>
                {t("保留观测段")}{" "}
                <select
                  aria-label={t("保留观测段")}
                  value={String(selectedRun[0].start_ms)}
                  onChange={e => setRun({ key: runKey, start: Number(e.target.value) })}
                >
                  {runs.map((r, i) => (
                    <option key={r[0].start_ms} value={r[0].start_ms}>
                      {i + 1} · {timestamp(ns(r[0].start_ms))} — {timestamp(ns(r.at(-1)!.start_ms))}{" "}
                      · {r.length}
                    </option>
                  ))}
                </select>
              </label>
              <p>{t("本段仅画 {rows} 根已观测分钟", { rows: selectedRun.length })}</p>
              <div className="minute-continuity-chart" key={`${runKey}:${selectedRun[0].start_ms}`}>
                {renderLive({ ...valid!, bars: selectedRun })}
              </div>
            </>
          ) : (
            <p role="status">{t("当前服务没有可用临时观测；旧实例响应不显示。")}</p>
          )}
          {!!split?.gaps.length && (
            <details open className="quote-data-note">
              <summary>{t("观测段间空档")}</summary>
              {split.gaps.map(g => (
                <p key={g.before_ms}>
                  {timestamp(ns(g.after_ms))} — {timestamp(ns(g.before_ms))} ·{" "}
                  {t("未观测标签窗 {minutes} 个（不等于缺交易分钟）", { minutes: g.minutes })}
                </p>
              ))}
            </details>
          )}
        </>
      )}
    </section>
  );
}
