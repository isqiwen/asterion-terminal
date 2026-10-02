import { useEffect, type ReactNode } from "react";
import { desktop, nativeDesktop, developmentEnvironment } from "@asterion/desktop-bridge/desktop";
import { translate, useLocale } from "../../i18n";
import "./window-frame.css";
export function WindowFrame({ children, title }: { children: ReactNode; title: string }) {
  useLocale();
  const windowTitle = developmentEnvironment
    ? `${title} · ${translate("host", "开发环境")}`
    : title;
  useEffect(() => {
    document.title = windowTitle;
    if (nativeDesktop) void desktop().setTitle(windowTitle).catch(console.error);
  }, [windowTitle]);
  return (
    <div className="window-frame">
      <div className="window-content">{children}</div>
    </div>
  );
}
