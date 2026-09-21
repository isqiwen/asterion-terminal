import React from "react";
import { createRoot } from "react-dom/client";
import { ApplicationHost } from "./extensions/ApplicationHost";
import { distribution } from "./distribution";
import "./theme/style.css";
import { Frame } from "./plugins/workflow/Frame";

createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <Frame>
      <ApplicationHost
        plugins={distribution}
        screen={
          new URLSearchParams(location.search).get("screen") === "settings"
            ? "terminal.settings"
            : "terminal.workspace"
        }
      />
    </Frame>
  </React.StrictMode>,
);
