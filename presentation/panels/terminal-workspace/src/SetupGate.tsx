import { invoke } from "@asterion/desktop-bridge/native";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
import { WindowFrame } from "@asterion/workbench/components/WindowFrame";
import { Setup, type SetupProgress } from "@asterion/workbench/startup/Setup";
import { useLanguage } from "@asterion/ui-kit/language";

const empty: SetupProgress = {
  running: false,
  ready: false,
  step: 0,
  downloaded: 0,
  network: null,
  dependencies: { total: null, installed: 0, phase: "", current: "" },
  total: null,
  error: "",
  directory: "",
};
export function SetupGate({ children }: { children: ReactNode }) {
  const [language, setLanguage] = useLanguage();
  const [entered, setEntered] = useState(!nativeDesktop);
  const [checking, setChecking] = useState(nativeDesktop);
  const [progress, setProgress] = useState(empty);
  const installing = useRef(false);
  useEffect(() => {
    if (!nativeDesktop) return;
    let active = true;
    void invoke<SetupProgress>("desktop_setup_status")
      .then(async (value) => {
        if (!active) return;
        setProgress(value);
        if (value.ready) setEntered(true);
        else await invoke("desktop_setup_window", { setup: true });
      })
      .catch((error) => {
        if (active) setProgress((p) => ({ ...p, error: String(error) }));
      })
      .finally(() => {
        if (active) setChecking(false);
      });
    return () => {
      active = false;
    };
  }, []);
  useEffect(() => {
    if (!progress.running) return;
    let active = true;
    const timer = setInterval(() => {
      void invoke<SetupProgress>("desktop_setup_status")
        .then((p) => {
          if (active) setProgress(p);
        })
        .catch(() => {
          /* The install command retains the final outcome. */
        });
    }, 400);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [progress.running]);
  const start = async () => {
    if (installing.current) return;
    installing.current = true;
    setProgress((p) => ({
      ...p,
      running: true,
      error: "",
      step: 0,
      total: null,
      downloaded: 0,
      network: null,
      dependencies: { total: null, installed: 0, phase: "", current: "" },
    }));
    try {
      setProgress(await invoke<SetupProgress>("desktop_setup_install"));
    } catch (error) {
      setProgress((p) => ({ ...p, running: false, error: String(error) }));
    } finally {
      installing.current = false;
    }
  };
  const enter = async () => {
    try {
      await invoke("desktop_setup_window", { setup: false });
      setEntered(true);
    } catch (error) {
      setProgress((p) => ({ ...p, error: String(error) }));
    }
  };
  if (entered) return children;
  return (
    <WindowFrame
      title={
        language === "zh"
          ? "Asterion Terminal — 首次设置"
          : "Asterion Terminal — First-Time Setup"
      }
      language={language}
    >
      <Setup
        progress={progress}
        language={language}
        onLanguage={setLanguage}
        checking={checking}
        onStart={() => void start()}
        onEnter={() => void enter()}
      />
    </WindowFrame>
  );
}
