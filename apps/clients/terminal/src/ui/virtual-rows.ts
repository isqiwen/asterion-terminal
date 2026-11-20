import { useVirtualizer } from "@tanstack/react-virtual";
import type { RefObject } from "react";

export const VIRTUALIZE_FROM = 80;

export function useVirtualRows(
  parentRef: RefObject<HTMLElement | null>,
  count: number,
  options?: { enabled?: boolean; rowHeight?: number; viewportHeight?: number },
) {
  const enabled = (options?.enabled ?? true) && count >= VIRTUALIZE_FROM;
  const rowHeight = options?.rowHeight ?? 30;
  const viewportHeight = options?.viewportHeight ?? 520;
  const virtualizer = useVirtualizer({
    count,
    getScrollElement: () => parentRef.current,
    estimateSize: () => rowHeight,
    overscan: 12,
    enabled,
    initialRect: { width: 800, height: viewportHeight },
  });
  if (!enabled) {
    return {
      virtualized: false,
      start: 0,
      end: count,
      topPad: 0,
      bottomPad: 0,
    };
  }
  const items = virtualizer.getVirtualItems();
  if (items.length === 0) {
    const end = Math.min(count, Math.ceil(viewportHeight / rowHeight) + 12);
    return {
      virtualized: true,
      start: 0,
      end,
      topPad: 0,
      bottomPad: Math.max(0, (count - end) * rowHeight),
    };
  }
  const start = items[0].index;
  const end = items[items.length - 1].index + 1;
  return {
    virtualized: true,
    start,
    end,
    topPad: items[0].start,
    bottomPad: Math.max(0, virtualizer.getTotalSize() - items[items.length - 1].end),
  };
}
