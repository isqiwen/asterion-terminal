import { useMemo, type ReactNode } from "react";
import { PriceChart } from "./PriceChart";

// The provider owns session slots, including minutes with no observation.
export type IntradaySlot = {
  label: string;
  price: string | null;
  average: string | null;
  volume: number | null;
};
export function IntradayChart({
  slots,
  reference,
  ticks,
  label,
  volumeLabel,
  averageLabel,
  referenceLabel,
  describe,
  formatPrice,
}: {
  slots: readonly IntradaySlot[];
  reference: string;
  ticks: readonly { index: number; label: string }[];
  label: string;
  volumeLabel: string;
  averageLabel: string;
  referenceLabel: string;
  describe: (index: number) => ReactNode;
  formatPrice?: (value: number) => string;
}) {
  const series = useMemo(
    () => ({
      kind: "line" as const,
      points: slots.map((slot, index) => ({
        timestamp_ns: String(index),
        price: slot.price,
        volume: slot.volume === null ? undefined : String(slot.volume),
      })),
    }),
    [slots],
  );
  const overlays = useMemo(
    () => [
      {
        label: averageLabel,
        color: "#e5b45a",
        values: slots.map(slot => (slot.average === null ? null : Number(slot.average))),
      },
    ],
    [slots, averageLabel],
  );
  const baseline = useMemo(
    () => ({ value: reference, label: referenceLabel }),
    [reference, referenceLabel],
  );
  return (
    <div className="intraday-chart">
      <PriceChart
        series={series}
        label={label}
        volumeLabel={volumeLabel}
        describe={describe}
        formatTime={value => slots[Number(value)].label}
        formatAxisTime={value =>
          ticks.find(tick => tick.index === Number(value))?.label ?? slots[Number(value)].label
        }
        formatPrice={formatPrice}
        overlays={overlays}
        referencePrice={baseline}
        fillArea
        fitHeight
      />
    </div>
  );
}
