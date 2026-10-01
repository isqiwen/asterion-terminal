import { useLayoutEffect, useRef, useState, type PointerEvent, type ReactNode } from "react";
import "./price-chart.css";

// Providers retain exact values and own fetching, subscriptions and aggregation.
// Numeric conversion here is exclusively for screen coordinates.
export type PricePoint = { timestamp_ns: string; price: string; volume?: string };
export type Candle = {
  timestamp_ns: string;
  open: string;
  high: string;
  low: string;
  close: string;
  volume: string;
};
type Series =
  { kind: "line"; points: readonly PricePoint[] } | { kind: "candles"; points: readonly Candle[] };
function localX(event: PointerEvent<SVGSVGElement>) {
  const matrix = event.currentTarget.getScreenCTM();
  return matrix
    ? new DOMPoint(event.clientX, event.clientY).matrixTransform(matrix.inverse()).x
    : 0;
}
const close = (point: PricePoint | Candle) => ("close" in point ? point.close : point.price);
export function PriceChart({
  series,
  label,
  volumeLabel,
  formatTime,
  formatAxisTime = formatTime,
  describe,
  controls,
  showVolume = true,
  compact = false,
  overlays = [],
  fillArea = false,
  fitHeight = false,
  oscillator,
  referencePrice,
  showPriceExtremes = false,
  mirrorPriceAxis = false,
  formatIndicatorValue = (value: number) => Number(value.toPrecision(8)).toString(),
}: {
  referencePrice?: { value: string; label: string };
  showPriceExtremes?: boolean;
  mirrorPriceAxis?: boolean;
  formatIndicatorValue?: (value: number) => string;
  oscillator?: {
    label: string;
    histogramLabel?: string;
    emptyLabel: string;
    lines: readonly { label: string; color: string; values: readonly (number | null)[] }[];
    histogram: readonly (number | null)[];
  };
  fitHeight?: boolean;
  fillArea?: boolean;
  overlays?: readonly { label: string; color: string; values: readonly (number | null)[] }[];
  showVolume?: boolean;
  compact?: boolean;
  controls?: { zoomIn: string; zoomOut: string; earlier: string; later: string; latest: string };
  series: Series;
  label: string;
  volumeLabel: string;
  formatTime: (value: string) => string;
  formatAxisTime?: (value: string) => string;
  describe: (index: number) => ReactNode;
}) {
  const frame = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(1000);
  const [plotHeight, setPlotHeight] = useState(0);
  const plot = useRef<SVGSVGElement>(null);
  const hasPoints = series.points.length > 0;
  // Measure before the first paint; observers maintain dimensions after resizing.
  useLayoutEffect(() => {
    const element = frame.current;
    if (!element) return;
    setWidth(Math.max(320, element.getBoundingClientRect().width));
    const observer = new ResizeObserver(entries => {
      setWidth(Math.max(320, entries[0].contentRect.width));
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, [hasPoints]);
  useLayoutEffect(() => {
    const element = plot.current;
    if (!fitHeight || !element) return;
    setPlotHeight(Math.max(190, element.getBoundingClientRect().height));
    const observer = new ResizeObserver(entries => {
      setPlotHeight(Math.max(190, entries[0].contentRect.height));
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, [fitHeight, hasPoints]);
  const [focus, setFocus] = useState<string | null>(null);
  const [windowSize, setWindowSize] = useState<number | null>(null);
  // Null follows the latest record. Browsing anchors to a timestamp so appends
  // cannot move the viewport. Owners remount on dataset/contract changes.
  const [anchor, setAnchor] = useState<string | null>(null);
  const drag = useRef<{ x: number; end: number } | null>(null);
  const points = series.points;
  const count = points.length;
  const size = Math.min(count, windowSize ?? count);
  const found = anchor === null ? count - 1 : points.findIndex(p => p.timestamp_ns === anchor);
  const end = Math.min(count, Math.max(size, found < 0 ? size : found + 1));
  const start = end - size;
  const visible = points.slice(start, end);
  if (!count) return null;
  // A rolling quote buffer changes indices, but a retained event keeps its identity.
  const focusedIndex = focus === null ? -1 : points.findIndex(p => p.timestamp_ns === focus);
  const index = Math.max(start, Math.min(end - 1, focusedIndex < 0 ? end - 1 : focusedIndex));
  const active = points[index];
  const overlayValues = overlays
    .flatMap(line => line.values.slice(start, end))
    .filter((value): value is number => value !== null && Number.isFinite(value));
  const rawHigh = Math.max(
    ...visible.map(p => Number("high" in p ? p.high : p.price)),
    ...overlayValues,
  );
  const rawLow = Math.min(
    ...visible.map(p => Number("low" in p ? p.low : p.price)),
    ...overlayValues,
  );
  const padding = rawHigh === rawLow ? Math.max(Math.abs(rawHigh) * 0.01, 0.00000001) : 0;
  const referenceValue = Number(referencePrice?.value);
  const reference = Number.isFinite(referenceValue) && referenceValue > 0 ? referenceValue : null;
  const radius =
    reference === null
      ? 0
      : Math.max(Math.abs(rawHigh - reference), Math.abs(rawLow - reference), reference * 0.001);
  const high = reference === null ? rawHigh + padding : reference + radius;
  const low = reference === null ? rawLow - padding : reference - radius;
  const span = high - low;
  const maxVolume = Math.max(1, ...visible.map(p => Number(p.volume ?? "0")));
  const bottom =
    fitHeight && plotHeight
      ? plotHeight - 20
      : showVolume
        ? compact
          ? 215
          : 300
        : compact
          ? 170
          : 235;
  const volumeHeight = fitHeight ? Math.max(40, bottom * 0.23) : compact ? 40 : 55;
  const oscillatorHeight = oscillator
    ? fitHeight
      ? bottom * 0.28
      : Math.min(85, bottom * 0.28)
    : 0;
  const volumeBottom = bottom - (oscillator ? oscillatorHeight + 24 : 0);
  const priceBottom = volumeBottom - (showVolume ? volumeHeight + 25 : 20);
  const oscillatorValues = oscillator
    ? [
        ...oscillator.histogram.slice(start, end),
        ...oscillator.lines.flatMap(line => line.values.slice(start, end)),
      ].filter((value): value is number => value !== null && Number.isFinite(value))
    : [];
  const oscillatorScale = Math.max(0.00000001, ...oscillatorValues.map(Math.abs));
  const oscillatorZero = bottom - oscillatorHeight / 2;
  const oscillatorY = (value: number) =>
    oscillatorZero - ((value / oscillatorScale) * oscillatorHeight) / 2;

  const axisWidth = Math.min(
    width * 0.45,
    Math.max(
      80,
      oscillatorValues.length
        ? String(Number((-oscillatorScale).toPrecision(5))).length * 7 + 16
        : 0,
      Math.max(
        ...Array.from(
          { length: 5 },
          (_, i) => String(Number((high - (i / 4) * (high - low)).toFixed(8))).length,
        ),
      ) *
        7 +
        16,
    ),
  );
  const left =
    reference === null && !mirrorPriceAxis
      ? 20
      : Math.min(
          width * 0.25,
          Math.max(
            40,
            ...Array.from(
              { length: 5 },
              (_, i) => String(Number((high - (i / 4) * span).toFixed(8))).length * 6 + 12,
            ),
          ),
        );
  const rightWidth =
    reference === null
      ? axisWidth
      : Math.min(
          width * 0.3,
          Math.max(
            64,
            `${((-radius / reference) * 100).toFixed(2)}%`.length * 6 + 16,
            String(Number(maxVolume.toPrecision(4))).length * 6 + 16,
          ),
        );
  const right = width - rightWidth;
  const step = (right - left - 10) / size;
  const x = (i: number) => left + 5 + step * (i + 0.5);
  const axisLabelWidth =
    Math.max(...visible.map(point => formatAxisTime(point.timestamp_ns).length)) * 7 + 16;
  const timeLabelCount = Math.min(
    5,
    size,
    Math.max(1, Math.floor((right - left) / axisLabelWidth)),
  );
  const timeIndices = Array.from({ length: timeLabelCount }, (_, i) =>
    timeLabelCount === 1 ? size - 1 : Math.round((i * (size - 1)) / (timeLabelCount - 1)),
  );
  const y = (value: string) => 25 + ((high - Number(value)) / span) * (priceBottom - 25);
  const pan = (next: number) => {
    const target = Math.max(size, Math.min(count, next));
    setAnchor(points[target - 1].timestamp_ns);
    setFocus(null);
  };
  const zoom = (factor: number) =>
    setWindowSize(Math.max(Math.min(5, count), Math.min(count, Math.round(size * factor))));
  return (
    <div className="price-chart" data-fit-height={fitHeight || undefined} ref={frame}>
      <p className="price-chart-cursor" aria-live="polite">
        {describe(index)}
      </p>
      {!!overlays.length && (
        <div className="price-chart-legend">
          {overlays.map(line => (
            <span
              key={line.label}
              style={{ color: line.color }}
              title={line.values[index]?.toString()}
            >
              {line.label}:{" "}
              {line.values[index] == null ? "—" : formatIndicatorValue(line.values[index]!)}
            </span>
          ))}
        </div>
      )}
      {controls && (
        <div className="price-chart-controls">
          <button
            aria-label={controls.earlier}
            disabled={start === 0}
            onClick={() => pan(end - Math.max(1, Math.round(size / 5)))}
          >
            ←
          </button>
          <button
            aria-label={controls.zoomIn}
            disabled={size <= Math.min(5, count)}
            onClick={() => zoom(0.8)}
          >
            +
          </button>
          <button
            aria-label={controls.zoomOut}
            disabled={size === count}
            onClick={() => zoom(1.25)}
          >
            −
          </button>
          <button
            aria-label={controls.later}
            disabled={end === count}
            onClick={() => pan(end + Math.max(1, Math.round(size / 5)))}
          >
            →
          </button>
          <button
            onClick={() => {
              setAnchor(null);
              setFocus(null);
            }}
          >
            {controls.latest}
          </button>
        </div>
      )}
      <svg
        ref={plot}
        viewBox={`0 0 ${width} ${bottom + 20}`}
        role="img"
        aria-label={label}
        tabIndex={0}
        data-visible-count={size}
        data-window-start={start}
        onKeyDown={event => {
          if (["ArrowLeft", "ArrowRight", "+", "=", "-", "Home", "End"].includes(event.key))
            event.preventDefault();
          if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
            const direction = event.key === "ArrowLeft" ? -1 : 1;
            if (event.shiftKey) pan(end + direction * Math.max(1, Math.round(size / 5)));
            else
              setFocus(points[Math.max(start, Math.min(end - 1, index + direction))].timestamp_ns);
          } else if (event.key === "+" || event.key === "=") zoom(0.8);
          else if (event.key === "-") zoom(1.25);
          else if (event.key === "Home") pan(size);
          else if (event.key === "End") {
            setAnchor(null);
            setFocus(null);
          }
        }}
        onPointerDown={event => {
          if (event.button !== 0) return;
          event.currentTarget.focus();
          event.currentTarget.setPointerCapture(event.pointerId);
          drag.current = { x: localX(event), end };
        }}
        onPointerMove={event => {
          const pointerX = localX(event);
          if (drag.current) {
            pan(drag.current.end - Math.round((pointerX - drag.current.x) / step));
          } else
            setFocus(
              points[
                start + Math.max(0, Math.min(size - 1, Math.floor((pointerX - left - 5) / step)))
              ].timestamp_ns,
            );
        }}
        onPointerUp={() => {
          drag.current = null;
        }}
        onLostPointerCapture={() => {
          drag.current = null;
        }}
        onPointerCancel={() => {
          drag.current = null;
        }}
      >
        {Array.from({ length: high === low ? 1 : 5 }, (_, tick) => {
          const ratio = tick / 4;
          const height = 25 + ratio * (priceBottom - 25);
          const value =
            reference === null ? high - ratio * span : reference + radius * (1 - ratio * 2);
          return (
            <g key={tick} className="price-chart-axis-tick">
              <path d={`M${left} ${height}H${right}`} stroke="var(--grid)" strokeDasharray="2 4" />
              <text
                x={reference === null ? right + 8 : left - 8}
                y={height + 4}
                textAnchor={reference === null ? "start" : "end"}
                style={
                  reference === null
                    ? undefined
                    : {
                        fill:
                          value > reference
                            ? "var(--quote-up)"
                            : value < reference
                              ? "var(--quote-down)"
                              : "var(--muted)",
                      }
                }
              >
                {Number(value.toFixed(8))}
              </text>
              {reference !== null && (
                <text
                  data-chart-percent="true"
                  x={right + 8}
                  y={height + 4}
                  style={{
                    fill:
                      value > reference
                        ? "var(--quote-up)"
                        : value < reference
                          ? "var(--quote-down)"
                          : "var(--muted)",
                  }}
                >
                  {value > reference ? "+" : ""}
                  {(((value - reference) / reference) * 100).toFixed(2)}%
                </text>
              )}
              {reference === null && mirrorPriceAxis && (
                <text x={left - 8} y={height + 4} textAnchor="end" data-mirrored-price="true">
                  {Number(value.toFixed(8))}
                </text>
              )}
            </g>
          );
        })}
        {reference !== null && (
          <path
            data-chart-reference={referencePrice!.value}
            d={`M${left} ${y(String(reference))}H${right}`}
            stroke="var(--muted)"
            strokeDasharray="5 5"
            opacity="0.65"
          >
            <title>
              {referencePrice!.label}: {referencePrice!.value}
            </title>
          </path>
        )}
        {showVolume && <path d={`M${left} ${volumeBottom}H${right}`} stroke="var(--grid)" />}
        {showVolume && (
          <g data-chart-volume-axis="true">
            <text x={left} y={volumeBottom - volumeHeight - 8}>
              {volumeLabel}
            </text>
            {[0, 0.5, 1].map(ratio => (
              <g key={ratio}>
                <path
                  d={`M${left} ${volumeBottom - ratio * volumeHeight}H${right}`}
                  stroke="var(--grid)"
                  strokeDasharray="2 4"
                />
                <text x={right + 8} y={volumeBottom - ratio * volumeHeight + 3}>
                  {Number((maxVolume * ratio).toPrecision(4))}
                </text>
              </g>
            ))}
          </g>
        )}
        {fillArea && series.kind === "line" && size > 1 && (
          <path
            d={`M${x(0)} ${priceBottom} ${visible.map((point, i) => `L${x(i)} ${y(close(point))}`).join(" ")} L${x(size - 1)} ${priceBottom} Z`}
            fill="var(--chart-area, #5278ce26)"
          />
        )}
        {series.kind === "line" &&
          (size === 1 ? (
            <circle cx={x(0)} cy={y(close(visible[0]))} r="3" fill="var(--accent)" />
          ) : (
            <polyline
              points={visible.map((p, i) => `${x(i)},${y(close(p))}`).join(" ")}
              fill="none"
              stroke="var(--price-line, var(--accent))"
              strokeWidth="1.5"
            />
          ))}
        {visible.map((p, i) => {
          const color =
            "open" in p
              ? Number(p.close) >= Number(p.open)
                ? "var(--quote-up)"
                : "var(--quote-down)"
              : start + i > 0 && Number(close(p)) < Number(close(points[start + i - 1]))
                ? "var(--quote-down)"
                : "var(--quote-up)";
          const width = Math.max(1, Math.min(20, step * 0.65));
          const height = (Number(p.volume ?? "0") / maxVolume) * volumeHeight;
          return (
            <g key={`${p.timestamp_ns}:${start + i}`}>
              {series.kind === "candles" && "open" in p && (
                <>
                  <path d={`M${x(i)} ${y(p.high)}V${y(p.low)}`} stroke={color} />
                  <rect
                    x={x(i) - width / 2}
                    y={Math.min(y(p.open), y(p.close))}
                    width={width}
                    height={Math.max(1, Math.abs(y(p.open) - y(p.close)))}
                    fill={Number(p.close) >= Number(p.open) ? "var(--canvas, #0f1115)" : color}
                    stroke={color}
                  />
                </>
              )}
              {showVolume && p.volume !== undefined && (
                <rect
                  data-chart-volume="true"
                  x={x(i) - width / 2}
                  y={volumeBottom - height}
                  width={width}
                  height={height}
                  fill={color}
                  opacity="0.55"
                />
              )}
            </g>
          );
        })}
        {showPriceExtremes &&
          series.kind === "candles" &&
          (["high", "low"] as const).map(field => {
            const candles = visible as readonly Candle[];
            const extreme = candles.reduce(
              (best, candle, i) =>
                (
                  field === "high"
                    ? Number(candle[field]) > Number(candles[best][field])
                    : Number(candle[field]) < Number(candles[best][field])
                )
                  ? i
                  : best,
              0,
            );
            const value = candles[extreme][field];
            const px = x(extreme);
            const py = y(value);
            const direction = px > (left + right) / 2 ? -1 : 1;
            return (
              <g key={field} data-price-extreme={field} pointerEvents="none">
                <path d={`M${px} ${py}h${direction * 10}`} stroke="var(--muted)" />
                <text
                  x={px + direction * 13}
                  y={Math.max(12, Math.min(priceBottom - 2, py + (field === "high" ? -4 : 12)))}
                  textAnchor={direction === 1 ? "start" : "end"}
                >
                  {value}
                </text>
              </g>
            );
          })}
        {overlays.map(line => {
          let drawing = false;
          const path = visible
            .map((_, i) => {
              const value = line.values[start + i];
              if (value == null || !Number.isFinite(value)) {
                drawing = false;
                return "";
              }
              const command = drawing ? "L" : "M";
              drawing = true;
              return `${command}${x(i)} ${y(String(value))}`;
            })
            .join(" ");
          return (
            <path
              key={line.label}
              data-indicator={line.label}
              d={path}
              fill="none"
              stroke={line.color}
              strokeWidth="1"
            />
          );
        })}
        {oscillator && (
          <g data-oscillator={oscillator.label}>
            <text
              x={left}
              y={bottom - oscillatorHeight - 8}
              data-oscillator-legend={oscillator.label}
            >
              <tspan>{oscillator.label} </tspan>
              {oscillator.lines.map(line => (
                <tspan key={line.label} fill={line.color}>
                  {line.label}:{" "}
                  {line.values[index] == null
                    ? "—"
                    : formatIndicatorValue(line.values[index]!)}{" "}
                </tspan>
              ))}
              <tspan
                fill={
                  (oscillator.histogram[index] ?? 0) >= 0 ? "var(--quote-up)" : "var(--quote-down)"
                }
              >
                {oscillator.histogramLabel ?? oscillator.label}:{" "}
                {oscillator.histogram[index] == null
                  ? "—"
                  : formatIndicatorValue(oscillator.histogram[index]!)}
              </tspan>
            </text>
            {oscillatorValues.length === 0 ? (
              <text
                data-oscillator-empty
                x={(left + right) / 2}
                y={oscillatorZero}
                textAnchor="middle"
              >
                {oscillator.emptyLabel}
              </text>
            ) : (
              <g data-oscillator-axis>
                <path
                  d={`M${left} ${oscillatorZero}H${right}`}
                  stroke="var(--grid)"
                  strokeDasharray="2 4"
                />
                {[-1, 0, 1].map(sign => (
                  <text
                    key={sign}
                    x={right + 8}
                    y={oscillatorY(sign * oscillatorScale) + (sign === -1 ? -2 : 4)}
                  >
                    {Number((sign * oscillatorScale).toPrecision(5))}
                  </text>
                ))}
                {visible.map((_, i) => {
                  const value = oscillator.histogram[start + i];
                  if (value == null || !Number.isFinite(value)) return null;
                  return (
                    <path
                      key={i}
                      d={`M${x(i)} ${oscillatorZero}V${oscillatorY(value)}`}
                      stroke={value >= 0 ? "var(--quote-up)" : "var(--quote-down)"}
                    />
                  );
                })}
                {oscillator.lines.map(line => {
                  let drawing = false;
                  const path = visible
                    .map((_, i) => {
                      const value = line.values[start + i];
                      if (value == null || !Number.isFinite(value)) {
                        drawing = false;
                        return "";
                      }
                      const command = drawing ? "L" : "M";
                      drawing = true;
                      return `${command}${x(i)} ${oscillatorY(value)}`;
                    })
                    .join(" ");
                  return (
                    <path
                      key={line.label}
                      data-oscillator-line={line.label}
                      d={path}
                      fill="none"
                      stroke={line.color}
                    />
                  );
                })}
              </g>
            )}
          </g>
        )}
        <path
          className="price-chart-crosshair"
          d={`M${x(index - start)} 15V${bottom} M${left} ${y(close(active))}H${right}`}
        />
        {timeIndices.map((pointIndex, i) => (
          <text
            key={pointIndex}
            data-chart-time
            x={
              size === 1
                ? x(pointIndex)
                : timeLabelCount === 1 || i === timeLabelCount - 1
                  ? right
                  : i === 0
                    ? left
                    : x(pointIndex)
            }
            y={bottom + 18}
            textAnchor={
              size === 1
                ? "middle"
                : timeLabelCount === 1 || i === timeLabelCount - 1
                  ? "end"
                  : i === 0
                    ? "start"
                    : "middle"
            }
          >
            {formatAxisTime(visible[pointIndex].timestamp_ns)}
          </text>
        ))}
      </svg>
    </div>
  );
}
