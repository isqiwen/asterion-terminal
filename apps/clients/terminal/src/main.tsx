import { nativeDesktop, desktop, developmentEnvironment } from "./bridge/desktop";
import "./host/workspace/titlebar.css";
import { translate, useLocale } from "./i18n";
import { StrictMode, useEffect, useState } from "react";
import { createRoot } from "react-dom/client";
import { Terminal } from "@asterion/terminal/App";
if (nativeDesktop && desktop().platform === "darwin") {
  document.documentElement.dataset.integratedTitlebar = "macos";
}
function EnvironmentLabel() {
  useLocale();
  const [stopping, setStopping] = useState(false);
  useEffect(() => (nativeDesktop ? desktop().onDevelopmentStopping(setStopping) : undefined), []);
  return developmentEnvironment ? (
    <span className="environment-label">
      {translate("host", stopping ? "正在停止开发环境…" : "开发环境")}
    </span>
  ) : null;
}
createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <div className="terminal-shell">
      <header className="terminal-titlebar">
        <div id="terminal-header-tools" />
        <EnvironmentLabel />
      </header>
      <div className="terminal-shell-content">
        <Terminal />
      </div>
    </div>
  </StrictMode>,
);
