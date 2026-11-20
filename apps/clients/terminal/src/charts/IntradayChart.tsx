import { useMemo, type ReactNode } from "react";
import { PriceChart, type ChartPriceLine } from "./PriceChart";

// The provider owns session slots, including minutes with no observation.
export type IntradaySlot = {
  label: string;
  price: string | null;
  average: string | null;
  volume: number | null;
  openInterest?: string | null;
};
export function IntradayChart({
  slots,
  reference,
  ticks,
  label,
  volumeLabel,
  openInterestLabel,
  averageLabel,
  referenceLabel,
  priceLines,
  describe,
  formatPrice,
  priceTick,
}: {
  slots: readonly IntradaySlot[];
  reference: string;
  ticks: readonly { index: number; label: string }[];
  label: string;
  volumeLabel: string;
  openInterestLabel?: string;
  averageLabel: string;
  referenceLabel: string;
  priceLines?: readonly ChartPriceLine[];
  describe: (index: number) => ReactNode;
  formatPrice?: (value: number) => string;
  priceTick?: string;
}) {
  const series = useMemo(
    () => ({
      kind: "line" as const,
      points: slots.map((slot, index) => ({
        timestamp_ns: String(index),
        price: slot.price,
        volume: slot.volume === null ? undefined : String(slot.volume),
        openInterest: slot.openInterest ?? undefined,
      })),
    }),
    [slots],
  );
  const overlays = useMemo(
    () => [
      {
        label: averageLabel,
        color: "var(--chart-average)",
        values: slots.map(slot => (slot.average === null ? null : Number(slot.average))),
      },
    ],
    [slots, averageLabel],
  );
  const baseline = useMemo(
    () => ({ value: reference, label: referenceLabel }),
    [reference, referenceLabel],
  );
  const numericReference = Number(reference);
  return (
    <div className="intraday-chart">
      <PriceChart
        series={series}
        label={label}
        volumeLabel={volumeLabel}
        openInterestLabel={openInterestLabel}
        describe={describe}
        formatTime={value => slots[Number(value)]?.label ?? ""}
        formatAxisTime={value =>
          ticks.find(tick => tick.index === Number(value))?.label ??
          slots[Number(value)]?.label ??
          ""
        }
        formatPrice={formatPrice}
        priceTick={priceTick}
        overlays={overlays}
        referencePrice={baseline}
        priceLines={priceLines}
        baselineValue={
          Number.isFinite(numericReference) && numericReference > 0 ? numericReference : undefined
        }
        fillArea
        fitHeight
      />
    </div>
  );
}
