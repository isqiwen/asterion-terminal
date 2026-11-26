import type {
  IChartApi,
  IPanePrimitive,
  IPanePrimitivePaneView,
  IPrimitivePaneRenderer,
  ISeriesApi,
  SeriesType,
  Time,
} from "lightweight-charts";
import { chartTime, type VerticalMarker } from "./price-chart-math";

export function verticalLinePrimitive(
  api: IChartApi,
  items: readonly VerticalMarker[],
  stroke: string,
  fill: string,
  text: string,
): IPanePrimitive<Time> {
  const renderer: IPrimitivePaneRenderer = {
    draw(target) {
      target.useMediaCoordinateSpace(({ context, mediaSize }) => {
        context.save();
        context.font = "500 11px ui-sans-serif, system-ui, sans-serif";
        context.textBaseline = "middle";
        for (const item of items) {
          const rawX = api.timeScale().timeToCoordinate(chartTime(item.index));
          if (rawX === null) continue;
          const x = Math.round(rawX) + 0.5;
          context.strokeStyle = stroke;
          context.lineWidth = 1;
          context.setLineDash([4, 3]);
          context.beginPath();
          context.moveTo(x, 0);
          context.lineTo(x, mediaSize.height);
          context.stroke();
          context.setLineDash([]);
          if (!item.label) continue;
          const padX = 6;
          const width = Math.ceil(context.measureText(item.label).width) + padX * 2;
          const height = 18;
          const left = x + 4 + width > mediaSize.width ? Math.max(2, x - width - 4) : x + 4;
          context.fillStyle = fill;
          context.strokeStyle = stroke;
          context.beginPath();
          context.roundRect(left, 6, width, height, 2);
          context.fill();
          context.stroke();
          context.fillStyle = text;
          context.fillText(item.label, left + padX, 6 + height / 2);
        }
        context.restore();
      });
    },
  };
  const views: readonly IPanePrimitivePaneView[] = [
    { zOrder: () => "top", renderer: () => renderer },
  ];
  return { paneViews: () => views };
}

export function priceExtremesPrimitive(
  api: IChartApi,
  candles: ISeriesApi<SeriesType>,
  items: readonly { index: number; price: string }[],
  color: string,
  fontFamily: string,
  fontSize: number,
): IPanePrimitive<Time> {
  const renderer: IPrimitivePaneRenderer = {
    draw(target) {
      target.useMediaCoordinateSpace(({ context, mediaSize }) => {
        context.save();
        context.font = `500 ${fontSize}px ${fontFamily}`;
        context.textBaseline = "middle";
        context.fillStyle = color;
        for (const item of items) {
          const x = api.timeScale().timeToCoordinate(chartTime(item.index));
          const y = candles.priceToCoordinate(Number(item.price));
          if (x === null || y === null) continue;
          const rightSide = x >= mediaSize.width / 2;
          const label = rightSide ? `${item.price} →` : `← ${item.price}`;
          const width = context.measureText(label).width;
          const clampedY = Math.max(8, Math.min(mediaSize.height - 8, y));
          const left = rightSide
            ? Math.max(4, x - width - 3)
            : Math.min(mediaSize.width - width - 4, x + 3);
          context.fillText(label, left, clampedY);
        }
        context.restore();
      });
    },
  };
  const views: readonly IPanePrimitivePaneView[] = [
    { zOrder: () => "top", renderer: () => renderer },
  ];
  return { paneViews: () => views };
}
