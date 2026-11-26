import { useEffect, useMemo, useRef, useState } from "react";
import { PriceChart } from "@asterion/terminal/charts/PriceChart";
import { decimal, groupedText } from "../../src/bridge/decimal";
import { tradingRequest } from "../../src/bridge/trading";
import type { DayClose, Funds, Session } from "../../src/generated/asterion/trading_pb";
import { notify } from "../../src/host/notifications";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { t } from "./live-shared";

export type PerformanceDay = {
  tradingDay: string;
  isLive: boolean;
  equity: number;
  preBalance: number;
  netFlow: number;
  dayPnl: number;
  dayReturn: number;
  nav: number;
  drawdown: number;
  commission: number;
};

export type PerformanceSummary = {
  days: PerformanceDay[];
  totalReturn: number;
  cumulativePnl: number;
  maxDrawdown: number;
  winRate: number | null;
  totalCommission: number;
  netCashFlow: number;
};

const num = (value?: { text: string }) => {
  const parsed = Number(value?.text ?? "");
  return Number.isFinite(parsed) ? parsed : 0;
};

const money = (value: number) =>
  groupedText(decimal((Math.abs(value) < 0.005 ? 0 : value).toFixed(2)));

const percent = (value: number | null) =>
  value === null || !Number.isFinite(value) ? "—" : `${(value * 100).toFixed(2)}%`;

const signedClass = (value: number) =>
  value > 0.005 ? "ctp-up" : value < -0.005 ? "ctp-down" : "";

const signedPctClass = (value: number) =>
  value > 1e-6 ? "ctp-up" : value < -1e-6 ? "ctp-down" : "";

const dayTimeNs = (tradingDay: string) =>
  String(BigInt(Date.parse(`${tradingDay}T07:00:00Z`)) * 1_000_000n);

const dayLabel = (timeNs: string) =>
  new Date(Number(BigInt(timeNs) / 1_000_000n) + 8 * 3600_000).toISOString().slice(0, 10);

// Cash-flow-adjusted daily NAV, drawdown, and P&L from archived DayClose records + live session.
export function buildPerformanceSeries(
  closes: readonly DayClose[],
  live?: Pick<Session, "tradingDay" | "funds">,
): PerformanceSummary {
  const byDay = new Map<string, { funds: Funds; isLive: boolean }>();
  for (const item of closes) {
    if (item.tradingDay && item.funds) {
      byDay.set(item.tradingDay, { funds: item.funds, isLive: false });
    }
  }
  if (
    live?.tradingDay &&
    live.funds &&
    (live.funds.dynamicEquity?.text || live.funds.balance?.text)
  ) {
    byDay.set(live.tradingDay, { funds: live.funds, isLive: true });
  }
  const ordered = [...byDay.entries()].sort(([a], [b]) => a.localeCompare(b));
  const days: PerformanceDay[] = [];
  let prevEquity = 0;
  let nav = 1;
  let peakNav = 1;
  let cumulativePnl = 0;
  let totalCommission = 0;
  let netCashFlow = 0;

  for (const [tradingDay, { funds, isLive }] of ordered) {
    const equity = num(funds.dynamicEquity) || num(funds.balance);
    const preBalance = num(funds.preBalance);
    const netFlow = num(funds.deposit) - num(funds.withdraw);
    const commission = num(funds.commission);
    const opening = preBalance > 0 ? preBalance : prevEquity;
    const base = opening + netFlow;
    const dayPnl = base > 0 ? equity - base : 0;
    const dayReturn = base > 0 ? dayPnl / base : 0;
    nav = Math.max(0, nav * (1 + dayReturn));
    peakNav = Math.max(peakNav, nav);
    const drawdown = peakNav > 0 ? Math.max(-1, nav / peakNav - 1) : 0;
    days.push({
      tradingDay,
      isLive,
      equity,
      preBalance: opening,
      netFlow,
      dayPnl,
      dayReturn,
      nav,
      drawdown,
      commission,
    });
    prevEquity = equity;
    cumulativePnl += dayPnl;
    totalCommission += commission;
    netCashFlow += netFlow;
  }

  const nonZero = days.filter(day => Math.abs(day.dayPnl) >= 0.005);
  const winRate = nonZero.length
    ? nonZero.filter(day => day.dayPnl > 0).length / nonZero.length
    : null;
  const totalReturn = days.length ? days[days.length - 1]!.nav - 1 : 0;
  const maxDrawdown = days.length ? Math.min(0, ...days.map(day => day.drawdown)) : 0;

  return {
    days,
    totalReturn,
    cumulativePnl,
    maxDrawdown,
    winRate,
    totalCommission,
    netCashFlow,
  };
}

export function AccountPerformance({
  connection,
  live,
  active,
}: {
  connection: string;
  live: Session;
  active: boolean;
}) {
  const [closes, setCloses] = useState<DayClose[]>([]);
  const [loading, setLoading] = useState(false);
  const [reloadTick, setReloadTick] = useState(0);
  const [focusedDay, setFocusedDay] = useState<string | null>(null);
  const cacheRef = useRef<{ connection: string; map: Map<string, DayClose> }>({
    connection: "",
    map: new Map(),
  });

  useEffect(() => {
    if (!active) return;
    let cancelled = false;
    if (cacheRef.current.connection !== connection) {
      cacheRef.current = { connection, map: new Map() };
      setCloses([]);
    }
    setLoading(true);
    void (async () => {
      try {
        const indexReply = await tradingRequest(connection, {
          case: "getDay",
          value: { tradingDay: "", offset: 0, limit: 1 },
        });
        if (cancelled || indexReply.result.case !== "getDay") return;
        const allDays = indexReply.result.value.tradingDays;
        const cache = cacheRef.current.map;
        const missing = allDays.filter(day => day !== live.tradingDay && !cache.has(day));
        const batchSize = 6;
        for (let i = 0; i < missing.length; i += batchSize) {
          if (cancelled) return;
          const chunk = missing.slice(i, i + batchSize);
          const replies = await Promise.all(
            chunk.map(tradingDay =>
              tradingRequest(connection, {
                case: "getDay",
                value: { tradingDay, offset: 0, limit: 1 },
              }),
            ),
          );
          if (cancelled) return;
          for (const reply of replies) {
            if (reply.result.case === "getDay" && reply.result.value.close?.tradingDay) {
              cache.set(reply.result.value.close.tradingDay, reply.result.value.close);
            }
          }
        }
        if (indexReply.result.value.close?.tradingDay) {
          const day = indexReply.result.value.close.tradingDay;
          if (day !== live.tradingDay) cache.set(day, indexReply.result.value.close);
        }
        const ordered = allDays
          .map(day => cache.get(day))
          .filter((item): item is DayClose => Boolean(item));
        setCloses(ordered);
      } catch (failure) {
        if (!cancelled) notify(failure, { namespace: "asterion.terminal.trading" });
      } finally {
        if (!cancelled) setLoading(false);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [active, connection, live.tradingDay, reloadTick]);

  const summary = useMemo(() => buildPerformanceSeries(closes, live), [closes, live]);

  const { navSeries, drawdownSeries } = useMemo(
    () => ({
      navSeries: {
        kind: "line" as const,
        points: summary.days.map(day => ({
          timestamp_ns: dayTimeNs(day.tradingDay),
          price: day.nav.toFixed(4),
        })),
      },
      drawdownSeries: {
        kind: "line" as const,
        points: summary.days.map(day => ({
          timestamp_ns: dayTimeNs(day.tradingDay),
          price: (day.drawdown * 100).toFixed(2),
        })),
      },
    }),
    [summary.days],
  );

  return (
    <div className="account-performance" role="region" aria-label={t("账户绩效")}>
      <div className="source-actions records-toolbar">
        <span className="subtle">
          {summary.days.length > 0
            ? t("已记录 {n} 个交易日（已剔除出入金影响）", { n: String(summary.days.length) })
            : loading
              ? t("正在加载历史净值…")
              : t("暂无历史绩效记录")}
        </span>
        <button
          type="button"
          disabled={loading}
          onClick={() => {
            cacheRef.current.map.clear();
            setReloadTick(tick => tick + 1);
          }}
        >
          {t("刷新")}
        </button>
      </div>
      {summary.days.length > 0 && (
        <>
          <dl className="paper-metrics" aria-label={t("实盘绩效汇总")}>
            <div>
              <dt>{t("累计收益率")}</dt>
              <dd className={signedPctClass(summary.totalReturn)}>
                {percent(summary.totalReturn)}
              </dd>
            </div>
            <div>
              <dt>{t("累计净盈亏")}</dt>
              <dd className={signedClass(summary.cumulativePnl)}>{money(summary.cumulativePnl)}</dd>
            </div>
            <div>
              <dt>{t("最大回撤")}</dt>
              <dd className={summary.maxDrawdown < -1e-6 ? "ctp-down" : undefined}>
                {percent(summary.maxDrawdown)}
              </dd>
            </div>
            <div>
              <dt>{t("盈利日占比")}</dt>
              <dd>{percent(summary.winRate)}</dd>
            </div>
            <div>
              <dt>{t("累计手续费")}</dt>
              <dd>{money(summary.totalCommission)}</dd>
            </div>
            <div>
              <dt>{t("净出入金")}</dt>
              <dd>{money(summary.netCashFlow)}</dd>
            </div>
          </dl>
          {summary.days.length >= 2 && (
            <div className="performance-charts">
              <PriceChart
                series={navSeries}
                label={t("累计净值")}
                volumeLabel=""
                titled
                showVolume={false}
                fillArea
                baselineValue={1}
                priceLines={[
                  {
                    value: "1",
                    label: "1.0000",
                    dashed: true,
                    axisLabelVisible: false,
                  },
                ]}
                focusedTime={focusedDay}
                onFocusChange={setFocusedDay}
                formatTime={dayLabel}
                describe={index => {
                  const row = summary.days[index]!;
                  return `${row.tradingDay} · ${row.nav.toFixed(4)} (${percent(row.dayReturn)})`;
                }}
              />
              <PriceChart
                series={drawdownSeries}
                label={t("回撤（%）")}
                volumeLabel=""
                titled
                showVolume={false}
                fillArea
                baselineValue={0}
                focusedTime={focusedDay}
                onFocusChange={setFocusedDay}
                formatTime={dayLabel}
                describe={index => {
                  const row = summary.days[index]!;
                  return `${row.tradingDay} · ${percent(row.drawdown)}`;
                }}
              />
            </div>
          )}
          <div className="paper-table" role="region" aria-label={t("每日净值明细")} tabIndex={0}>
            <table aria-label={t("每日净值明细")}>
              <thead>
                <tr>
                  <th scope="col">{t("交易日")}</th>
                  <th scope="col" className="numeric">
                    {t("盯市净值")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("当日盈亏")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("日收益率")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("累计净值")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("回撤")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("手续费")}
                  </th>
                  <th scope="col" className="numeric">
                    {t("净出入金")}
                  </th>
                </tr>
              </thead>
              <tbody>
                {summary.days
                  .slice()
                  .reverse()
                  .map(row => (
                    <tr key={row.tradingDay}>
                      <td>
                        <span className="performance-day-cell">
                          <span>{row.tradingDay}</span>
                          {row.isLive && <StatusBadge tone="neutral">{t("盘中")}</StatusBadge>}
                        </span>
                      </td>
                      <td className="numeric">{money(row.equity)}</td>
                      <td className={`numeric ${signedClass(row.dayPnl)}`}>{money(row.dayPnl)}</td>
                      <td className={`numeric ${signedPctClass(row.dayReturn)}`}>
                        {percent(row.dayReturn)}
                      </td>
                      <td className="numeric">{row.nav.toFixed(4)}</td>
                      <td className={`numeric ${row.drawdown < -1e-6 ? "ctp-down" : ""}`}>
                        {percent(row.drawdown)}
                      </td>
                      <td className="numeric">{money(row.commission)}</td>
                      <td className="numeric">{money(row.netFlow)}</td>
                    </tr>
                  ))}
              </tbody>
            </table>
          </div>
        </>
      )}
    </div>
  );
}
