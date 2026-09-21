import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { useEffect, useState, type ReactNode } from "react";
import { WindowFrame } from "../../components/WindowFrame";
import { nativeDesktop } from "../../deployment/desktop";
import {
  useLanguage,
  windowTitle,
  type WindowKind,
} from "../identity/language";

export function Frame({ children }: { children: ReactNode }) {
  const [language] = useLanguage();
  const [kind, setKind] = useState<WindowKind>(() =>
    new URLSearchParams(location.search).get("screen") === "settings"
      ? "settings"
      : nativeDesktop && getCurrentWindow().label.startsWith("workspace-")
        ? "workspace"
        : "main",
  );
  const title = windowTitle(language, kind);
  useEffect(() => {
    if (!nativeDesktop || !getCurrentWindow().label.startsWith("workspace-"))
      return;
    let active = true;
    void invoke<{ layout: { kind: "workspace" | "chart" } }>(
      "desktop_workspace_read",
    )
      .then(({ layout }) => {
        if (active) setKind(layout.kind === "chart" ? "chart" : "workspace");
      })
      .catch(console.error);
    return () => {
      active = false;
    };
  }, []);
  return (
    <WindowFrame title={title} language={language}>
      {children}
    </WindowFrame>
  );
}
