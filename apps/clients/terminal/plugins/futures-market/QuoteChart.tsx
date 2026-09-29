import { PriceChart } from "@asterion/client-ui/PriceChart";
import { timestamp, type LiveMarket } from "../../src/bridge/client";
import { translate } from "../contract";
import { chartTime } from "./chart-time";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
export type QuoteRow = LiveMarket["subscriptions"][number] & { displayName?: string };
export const quoteId = (row: QuoteRow) => `${row.venue}.${row.symbol}`;
export function QuoteChart({ market, row }: { market: LiveMarket; row: QuoteRow }) {
  const history = market.history;
  const points =
    history?.points.filter(p => p.venue === row.venue && p.symbol === row.symbol) ?? [];
  const available = market.transport_online && market.phase === "connected" && history?.available;
  return (
    <section className="contract-live-chart" aria-label={t("最近报价走势")}>
      {available && points.length ? (
        <PriceChart
          compact
          fitHeight
          fillArea
          key={`${quoteId(row)}:${history.stream_id}:${history.generation}`}
          referencePrice={
            row.quote?.previous_settlement
              ? { value: row.quote.previous_settlement, label: t("昨结价") }
              : undefined
          }
          series={{ kind: "line", points }}
          label={t("最近报价走势")}
          showVolume={points.some(p => p.volume !== undefined)}
          volumeLabel={t("成交量")}
          formatTime={timestamp}
          formatAxisTime={ns => chartTime(ns, false, true)}
          describe={index => (
            <>
              {timestamp(points[index].timestamp_ns)} · {points[index].price}
              {points[index].volume !== undefined &&
                ` · ${t("区间成交量")} ${points[index].volume}`}
            </>
          )}
          controls={{
            zoomIn: t("放大"),
            zoomOut: t("缩小"),
            earlier: t("向前"),
            later: t("向后"),
            latest: t("最新数据"),
          }}
        />
      ) : (
        <p className="quote-empty" role="status">
          {t(available ? "等待报价事件" : "报价序列暂不可用")}
        </p>
      )}
      {history?.interrupted && (
        <p className="quote-data-note">{t("序列曾中断，仅显示恢复后的可用记录。")}</p>
      )}
      <details className="quote-data-note">
        <summary>{t("数据说明")}</summary>
        <p>{t("最近最多 512 条跨合约报价事件；非完整分时，不代表逐笔成交或分钟 K 线。")}</p>
      </details>
    </section>
  );
}
