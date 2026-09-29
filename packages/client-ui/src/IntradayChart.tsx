import { useLayoutEffect, useRef, useState, type PointerEvent, type ReactNode } from "react";
import "./price-chart.css";

// One slot per session minute. Providers own session layout and aggregation;
// values here are converted to numbers only for screen coordinates.
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
  describe,
  formatPrice = value => String(value),
}: {
  slots: readonly IntradaySlot[];
  // Previous settlement: the zero line of the percent axis.
  reference: string;
  // Slot positions that receive a time label.
  ticks: readonly { index: number; label: string }[];
  label: string;
  describe: (index: number) => ReactNode;
  formatPrice?: (value: number) => string;
}) {
  const frame = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ width: 480, height: 320 });
  useLayoutEffect(() => {
    const element = frame.current;
    if (!element) return;
    const measure = (width: number, height: number) =>
      setSize({ width: Math.max(240, width), height: Math.max(180, height) });
    const box = element.getBoundingClientRect();
    measure(box.width, box.height);
    const observer = new ResizeObserver(entries =>
      measure(entries[0].contentRect.width, entries[0].contentRect.height),
    );
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  const [focus, setFocus] = useState<number | null>(null);
  const base = Number(reference);
  const { width, height } = size;
  const left = 56,
    right = width - 56,
    top = 8,
    axis = height - 18,
    priceBottom = top + (axis - top) * 0.72,
    volumeTop = priceBottom + 14;
  const count = Math.max(1, slots.length);
  const x = (index: number) => left + ((index + 0.5) / count) * (right - left);
  const values = slots.flatMap(slot =>
    [slot.price, slot.average]
      .filter((value): value is string => value !== null)
      .map(Number)
      .filter(Number.isFinite),
  );
  const deviation = Math.max(...values.map(value => Math.abs(value - base)), 0);
  const span = deviation > 0 ? deviation * 1.08 : Math.max(Math.abs(base) * 0.01, 1);
  const y = (value: number) => top + ((base + span - value) / (2 * span)) * (priceBottom - top);
  const maxVolume = Math.max(1, ...slots.map(slot => slot.volume ?? 0));
  const volumeY = (value: number) => axis - (value / maxVolume) * (axis - volumeTop);
  const path = (pick: (slot: IntradaySlot) => string | null) => {
    let d = "";
    let open = false;
    slots.forEach((slot, index) => {
      const value = pick(slot);
      if (value === null) {
        open = false;
        return;
      }
      d += `${open ? "L" : "M"}${x(index).toFixed(1)} ${y(Number(value)).toFixed(1)}`;
      open = true;
    });
    return d;
  };
  const priced = slots.map((slot, index) => (slot.price === null ? -1 : index));
  const first = priced.find(index => index >= 0) ?? -1;
  const last = priced.reduce((found, index) => Math.max(found, index), -1);
  const area =
    first >= 0 && slots.slice(first, last + 1).every(slot => slot.price !== null)
      ? `${path(slot => slot.price)}L${x(last).toFixed(1)} ${priceBottom}L${x(first).toFixed(1)} ${priceBottom}Z`
      : "";
  const levels = [1, 0.5, 0, -0.5, -1].map(step => base + step * span);
  const tone = (value: number) =>
    value > base ? "var(--quote-up)" : value < base ? "var(--quote-down)" : "var(--muted)";
  const locate = (event: PointerEvent<SVGSVGElement>) => {
    const matrix = event.currentTarget.getScreenCTM();
    const local = matrix
      ? new DOMPoint(event.clientX, event.clientY).matrixTransform(matrix.inverse()).x
      : 0;
    const index = Math.floor(((local - left) / (right - left)) * count);
    setFocus(index >= 0 && index < count ? index : null);
  };
  const active = focus ?? (last >= 0 ? last : null);
  return (
    <div className="intraday-chart">
      <div className="price-chart-cursor" aria-live="polite">
        {active !== null && describe(active)}
      </div>
      <div className="intraday-chart-frame" ref={frame}>
        <svg
          viewBox={`0 0 ${width} ${height}`}
          width={width}
          height={height}
          role="img"
          aria-label={label}
          onPointerMove={locate}
          onPointerLeave={() => setFocus(null)}
        >
          <defs>
            <linearGradient id="intraday-area" x1="0" x2="0" y1="0" y2="1">
              <stop offset="0" stopColor="var(--intraday-line, #4f7bd9)" stopOpacity="0.42" />
              <stop offset="1" stopColor="var(--intraday-line, #4f7bd9)" stopOpacity="0.04" />
            </linearGradient>
          </defs>
          {levels.map((value, index) => (
            <g key={index}>
              <path
                d={`M${left} ${y(value)}H${right}`}
                stroke="var(--grid)"
                strokeDasharray={index === 2 ? undefined : "2 4"}
              />
              <text x={left - 6} y={y(value) + 4} textAnchor="end" style={{ fill: tone(value) }}>
                {formatPrice(value)}
              </text>
              <text x={right + 6} y={y(value) + 4} style={{ fill: tone(value) }}>
                {base > 0 ? `${(((value - base) / base) * 100).toFixed(2)}%` : ""}
              </text>
            </g>
          ))}
          <path d={`M${left} ${top}V${axis}M${right} ${top}V${axis}`} stroke="var(--grid)" />
          <path
            d={`M${left} ${volumeTop - 6}H${right}M${left} ${axis}H${right}`}
            stroke="var(--grid)"
          />
          {ticks.map(({ index, label }, position) => (
            <g key={`${index}:${label}`}>
              <path d={`M${x(index)} ${top}V${axis}`} stroke="var(--grid)" strokeDasharray="2 4" />
              <text
                x={x(index)}
                y={height - 4}
                textAnchor={
                  position === 0 ? "start" : position === ticks.length - 1 ? "end" : "middle"
                }
              >
                {label}
              </text>
            </g>
          ))}
          {area && <path d={area} fill="url(#intraday-area)" />}
          <path
            d={path(slot => slot.price)}
            fill="none"
            stroke="var(--intraday-line, #4f7bd9)"
            strokeWidth="1.2"
          />
          <path
            d={path(slot => slot.average)}
            fill="none"
            stroke="var(--intraday-average, #e5b45a)"
            strokeWidth="1.2"
          />
          {slots.map((slot, index) => {
            if (!slot.volume) return null;
            let previous: string | null = null;
            for (let before = index - 1; before >= 0 && previous === null; --before)
              previous = slots[before].price;
            const color =
              slot.price !== null && previous !== null && Number(slot.price) < Number(previous)
                ? "var(--quote-down)"
                : "var(--quote-up)";
            return (
              <path
                key={index}
                d={`M${x(index).toFixed(1)} ${axis}V${volumeY(slot.volume).toFixed(1)}`}
                stroke={color}
                strokeWidth={Math.max(1, ((right - left) / count) * 0.6)}
              />
            );
          })}
          {focus !== null && (
            <path
              className="price-chart-crosshair"
              d={`M${x(focus)} ${top}V${axis}${
                slots[focus]?.price !== null
                  ? `M${left} ${y(Number(slots[focus].price))}H${right}`
                  : ""
              }`}
            />
          )}
        </svg>
      </div>
    </div>
  );
}
