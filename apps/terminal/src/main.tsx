import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Terminal } from "@asterion/terminal/App";
createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <Terminal />
  </StrictMode>,
);
