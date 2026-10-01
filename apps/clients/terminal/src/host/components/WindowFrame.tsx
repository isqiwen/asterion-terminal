import { useEffect, type ReactNode } from "react";
import { desktop, nativeDesktop } from "@asterion/desktop-bridge/desktop";
import "./window-frame.css";
export function WindowFrame({ children, title }: { children: ReactNode; title: string }) {
  useEffect(() => {
    document.title = title;
    if (nativeDesktop) void desktop().setTitle(title).catch(console.error);
  }, [title]);
  return (
    <div className="window-frame">
      <div className="window-content">{children}</div>
    </div>
  );
}
