import { getCurrentWindow } from "@tauri-apps/api/window";
import { useEffect, useState, type ReactNode } from "react";
import { nativeDesktop } from "../deployment/desktop";
import "./window-frame.css";

const edges = [
  "North",
  "South",
  "East",
  "West",
  "NorthEast",
  "NorthWest",
  "SouthEast",
  "SouthWest",
] as const;

export function WindowFrame({
  children,
  title,
  language,
}: {
  children: ReactNode;
  title: string;
  language: string;
}) {
  const [custom, setCustom] = useState(false);
  const zh = language === "zh";
  useEffect(() => {
    document.title = title;
    if (nativeDesktop)
      void getCurrentWindow().setTitle(title).catch(console.error);
  }, [title]);

  const [maximized, setMaximized] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    if (!nativeDesktop) return;
    const win = getCurrentWindow();
    let active = true;
    let unlisten: (() => void) | undefined;
    const refresh = async () => {
      const value = await win.isMaximized();
      if (active) setMaximized(value);
    };
    void (async () => {
      const decorated = await win.isDecorated();
      if (!active || decorated) return;
      setCustom(true);
      await refresh();
      const dispose = await win.onResized(() => {
        void refresh().catch(console.error);
      });
      if (active) unlisten = dispose;
      else dispose();
    })().catch(console.error);
    return () => {
      active = false;
      unlisten?.();
    };
  }, []);

  const control = (action: () => Promise<unknown>) => {
    setError("");
    void action().catch((reason) => setError(String(reason)));
  };

  return (
    <div className={`window-frame${custom ? " window-frame-custom" : ""}`}>
      {custom && (
        <header className="window-titlebar" key="titlebar">
          <div className="window-drag" data-tauri-drag-region>
            <span className="window-mark" aria-hidden="true">
              ✧
            </span>
            <span>{title}</span>
            {error && (
              <span role="alert" className="window-error">
                {error}
              </span>
            )}
          </div>
          <div className="window-controls">
            <button
              aria-label={zh ? "最小化" : "Minimize"}
              title={zh ? "最小化" : "Minimize"}
              onClick={() => control(() => getCurrentWindow().minimize())}
            >
              <svg viewBox="0 0 12 12" aria-hidden="true">
                <path d="M2 9h8" />
              </svg>
            </button>
            <button
              aria-label={
                maximized
                  ? zh
                    ? "还原窗口"
                    : "Restore"
                  : zh
                    ? "最大化"
                    : "Maximize"
              }
              title={
                maximized
                  ? zh
                    ? "还原窗口"
                    : "Restore"
                  : zh
                    ? "最大化"
                    : "Maximize"
              }
              onClick={() => control(() => getCurrentWindow().toggleMaximize())}
            >
              <svg viewBox="0 0 12 12" aria-hidden="true">
                {maximized ? (
                  <path d="M4 2h6v6M2 4h6v6H2z" />
                ) : (
                  <path d="M2 2h8v8H2z" />
                )}
              </svg>
            </button>
            <button
              className="window-close"
              aria-label={zh ? "关闭窗口" : "Close"}
              title={zh ? "关闭窗口" : "Close"}
              onClick={() => control(() => getCurrentWindow().close())}
            >
              <svg viewBox="0 0 12 12" aria-hidden="true">
                <path d="m2 2 8 8m0-8-8 8" />
              </svg>
            </button>
          </div>
        </header>
      )}
      <div className="window-content" key="content">
        {children}
      </div>
      {custom &&
        !maximized &&
        edges.map((direction) => (
          <div
            key={direction}
            aria-hidden="true"
            className="window-resize"
            data-edge={direction}
            onPointerDown={(event) => {
              if (event.button === 0)
                control(() =>
                  getCurrentWindow().startResizeDragging(direction),
                );
            }}
          />
        ))}
    </div>
  );
}
