import { nativeDesktop, desktop, developmentEnvironment } from "./bridge/desktop";
import "./host/workspace/titlebar.css";
import { translate, useLocale } from "./i18n";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Terminal } from "@asterion/terminal/App";
// The page's title bar is the window's: macOS insets the window buttons at its
// left, Linux overlays them at its right.
if (nativeDesktop) {
  document.documentElement.dataset.integratedTitlebar =
    desktop().platform === "darwin" ? "macos" : "overlay";
}
function EnvironmentLabel() {
  useLocale();
  return developmentEnvironment ? (
    <span className="environment-label">{translate("host", "开发环境")}</span>
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
