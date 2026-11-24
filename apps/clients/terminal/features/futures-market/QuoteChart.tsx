import { useMemo } from "react";
import { PriceChart, type ChartPriceLine } from "@asterion/terminal/charts/PriceChart";
import { timestamp } from "../../src/bridge/client";
import { FeedState, type QuoteRow } from "../../src/generated/asterion/market_pb";
import { contractGeneration, rowKey, type MarketView } from "../../src/host/market";
import { translate } from "../../src/i18n";
import { chartTime } from "./chart-time";
import { priceTickOf } from "./price-tick";
const t = (key: string) => translate("asterion.terminal.futures-market", key);
const emptyPoints: readonly { timestamp_ns: string; price: string; volume?: string }[] = [];
export function QuoteChart({
  market,
  row,
  priceLines,
}: {
  market: MarketView;
  row: QuoteRow;
  priceLines?: readonly ChartPriceLine[];
}) {
  const points = market.ticks.get(rowKey(row)) ?? emptyPoints;
  const reference = row.quote?.previousSettlement?.text;
  const validReference =
    reference && Number.isFinite(Number(reference)) && Number(reference) > 0
      ? Number(reference)
      : undefined;
  const referenceLabel = t("昨结价");
  const referencePrice = useMemo(
    () => (validReference !== undefined ? { value: reference!, label: referenceLabel } : undefined),
    [validReference, reference, referenceLabel],
  );
  const series = useMemo(() => ({ kind: "line" as const, points }), [points]);
  const showVolume = useMemo(() => points.some(p => p.volume !== undefined), [points]);
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
          key={`${rowKey(row)}:${contractGeneration(market, rowKey(row))}`}
          referencePrice={referencePrice}
          baselineValue={validReference}
          priceLines={priceLines}
          series={series}
          label={t("最近报价走势")}
          showVolume={showVolume}
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
