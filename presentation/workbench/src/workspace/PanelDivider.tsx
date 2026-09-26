import type { PointerEvent } from "react";
export function PanelDivider({
  horizontal = false,
  reverse = false,
  ratio,
  onChange,
}: {
  horizontal?: boolean;
  reverse?: boolean;
  ratio: number;
  onChange: (ratio: number) => void;
}) {
  function drag(e: PointerEvent<HTMLDivElement>) {
    const target = e.currentTarget;
    const rect = target.parentElement!.getBoundingClientRect();
    target.setPointerCapture(e.pointerId);
    const move = (event: globalThis.PointerEvent) =>
      onChange(
        Math.max(
          30,
          Math.min(
            75,
            (reverse ? 100 : 0) +
              (reverse ? -1 : 1) *
                (horizontal
                  ? (100 * (event.clientY - rect.top)) / rect.height
                  : (100 * (event.clientX - rect.left)) / rect.width),
          ),
        ),
      );
    const end = () => {
      target.removeEventListener("pointermove", move);
      target.removeEventListener("pointerup", end);
      target.removeEventListener("pointercancel", end);
    };
    target.addEventListener("pointermove", move);
    target.addEventListener("pointerup", end);
    target.addEventListener("pointercancel", end);
  }
  return (
    <div
      className="panel-divider"
      role="separator"
      aria-label="行情与图表分隔线"
      aria-orientation={horizontal ? "horizontal" : "vertical"}
      aria-valuemin={30}
      aria-valuemax={75}
      aria-valuenow={Math.round(ratio)}
      tabIndex={0}
      onPointerDown={drag}
      onKeyDown={(e) => {
        if (
          ["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home"].includes(
            e.key,
          )
        ) {
          e.preventDefault();
          onChange(
            e.key === "Home"
              ? 60
              : Math.max(
                  30,
                  Math.min(
                    75,
                    ratio +
                      (["ArrowLeft", "ArrowUp"].includes(e.key) ? -2 : 2) *
                        (reverse ? -1 : 1),
                  ),
                ),
          );
        }
      }}
    />
  );
}
