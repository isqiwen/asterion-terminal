import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./app/App";
import { Settings } from "./settings/Settings";
import "./theme/style.css";

createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    {new URLSearchParams(location.search).get("screen") === "settings" ? (
      <Settings />
    ) : (
      <App />
    )}
  </React.StrictMode>,
);
