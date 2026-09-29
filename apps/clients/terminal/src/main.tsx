import { nativeDesktop, desktop } from "./bridge/desktop";
import "./host/workspace/titlebar.css";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Terminal } from "@asterion/terminal/App";
if (nativeDesktop && desktop().platform === "darwin") {
  document.documentElement.dataset.integratedTitlebar = "macos";
}
createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <div className="terminal-shell">
      <header className="terminal-titlebar">
        <div id="terminal-header-tools" />
      </header>
      <div className="terminal-shell-content">
        <Terminal />
      </div>
    </div>
  </StrictMode>,
);
