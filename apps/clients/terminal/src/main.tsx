import { nativeDesktop, desktop } from "./bridge/desktop";
import "./host/workspace/titlebar.css";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Terminal } from "@asterion/terminal/App";
// The page's title bar is the window's: macOS insets the window buttons at its
// left, Linux overlays them at its right.
function reportError(error: unknown) {
  const stack = error instanceof Error ? error.stack || error.message : String(error);
  console.error(stack);
}
window.addEventListener("error", event => reportError(event.error || event.message));
window.addEventListener("unhandledrejection", event => reportError(event.reason));
if (nativeDesktop) {
  document.documentElement.dataset.integratedTitlebar =
    desktop().platform === "darwin" ? "macos" : "overlay";
}
createRoot(document.getElementById("root")!, { onUncaughtError: reportError }).render(
  <StrictMode>
    <div className="terminal-shell">
      <header className="terminal-titlebar" />
      <div className="terminal-shell-content">
        <Terminal />
      </div>
    </div>
  </StrictMode>,
);
