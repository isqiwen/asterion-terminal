import { PriceChart } from "@asterion/terminal/charts/PriceChart";
import { timestamp } from "../../src/bridge/client";
import { FeedState, type QuoteRow } from "../../src/generated/asterion/market_pb";
import { rowKey, type MarketView } from "../../src/host/market";
import { translate } from "../../src/i18n";
import { chartTime } from "./chart-time";
import { priceTickOf } from "./price-tick";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
export function QuoteChart({ market, row }: { market: MarketView; row: QuoteRow }) {
  const points = market.ticks.get(rowKey(row)) ?? [];
  const reference = row.quote?.previousSettlement?.text;
  const available = market.online && market.board.feed?.state === FeedState.CONNECTED;
  return (
    <section
      className="contract-live-chart"
      aria-label={t("最近报价走势")}
      title={t("每个合约最近 512 条报价，每 0.25 秒一帧")}
    >
      {available && points.length ? (
        <PriceChart
          compact
          fitHeight
          fillArea
          priceTick={priceTickOf(market, row.instrument!.exchange, row.instrument!.symbol)}
          key={`${rowKey(row)}:${market.generation}`}
          referencePrice={
            reference && Number.isFinite(Number(reference)) && Number(reference) > 0
              ? { value: reference, label: t("昨结价") }
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
      {market.interrupted && (
        <p className="quote-data-note">{t("序列曾中断，只显示恢复后的报价")}</p>
      )}
    </section>
  );
}
