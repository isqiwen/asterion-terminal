import { BrandMark } from "../ui/BrandMark";
import { asDisplayError, type DisplayError } from "../i18n/errors";
import { translate } from "../i18n";
const t = (key: string) => translate("host", key);
import { useLocale } from "../i18n";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { WindowFrame } from "../host/components/WindowFrame";
import { NotificationCenter, notify, useErrorNotification } from "../host/notifications";
import { request, type PythonRuntime } from "../bridge/client";
import { StartupScreen } from "./StartupScreen";
import "./setup.css";

const megabytes = (bytes: number) => `${(bytes / 2 ** 20).toFixed(0)} MB`;

// Lab starts behind the startup screen after its runtime is ready.
export function SetupGate({ children }: { children: ReactNode }) {
  const { locale, setLocale } = useLocale();
  const language = locale === "zh-CN" ? "zh" : "en";
  const [runtime, setRuntime] = useState<PythonRuntime | null>(null);
  const [phase, setPhase] = useState<"starting" | "beneath" | "leaving" | "entered">("starting");
  const [error, setError] = useState<DisplayError>("");
  const failed = runtime?.state === "failed";
  useErrorNotification(
    error || (failed ? runtime.error : runtime?.state === "ready" ? "" : undefined),
  );
  const started = useRef(false);
  const enter = () => {
    if (started.current) return;
    started.current = true;
    // A failed Lab still leaves the other workspaces usable.
    void request("lab.endpoint")
      .catch(reason => notify(reason, { namespace: "asterion.terminal.research" }))
      .then(() => setPhase("entered"));
  };
  const read = async (install: boolean) => {
    const reply = await request(install ? "runtime.python.install" : "runtime.python.status");
    const value = reply.python_runtime ?? null;
    setRuntime(value);
    return value;
  };
  useEffect(() => {
    let active = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const poll = async (install: boolean) => {
      try {
        const value = await read(install);
        if (!active || !value) return;
        if (value.state === "ready") enter();
        else if (value.state === "missing") void poll(true);
        else if (value.state === "installing") timer = setTimeout(() => void poll(false), 500);
      } catch (reason) {
        if (active) setError(asDisplayError(reason));
      }
    };
    void poll(false);
    return () => {
      active = false;
      clearTimeout(timer);
    };
    // Runs once per launch; a retry polls again through retry().
  }, []);
  const retry = async () => {
    setError("");
    try {
      let value = await read(true);
      while (value?.state === "installing") {
        await new Promise(resolve => setTimeout(resolve, 500));
        value = await read(false);
      }
      if (value?.state === "ready") enter();
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  };
  if ((!runtime || runtime.state === "ready") && !error)
    // One tree throughout, so the workbench is never mounted twice.
    return (
      <div className="startup-stage">
        {phase !== "starting" && <div className={`startup-workbench ${phase}`}>{children}</div>}
        {phase !== "entered" && (
          <div className="startup-cover">
            <StartupScreen leaving={phase === "leaving"} />
          </div>
        )}
      </div>
    );
  return (
    <WindowFrame title={t("Asterion Terminal — 首次启动")}>
      <div className="terminal">
        <main className="first-setup" aria-label={t("首次启动")}>
          <div className="setup-language-control">
            <select
              className="setup-language"
              aria-label={t("语言")}
              value={language}
              onChange={event => {
                try {
                  setLocale(event.target.value === "zh" ? "zh-CN" : "en-US");
                } catch {
                  setError(t("保存失败，请检查本机存储权限。"));
                }
              }}
            >
              <option value="zh">{t("简体中文")}</option>
              <option value="en">English</option>
            </select>
          </div>
          <section className="setup-content">
            <header>
              <div className="setup-mark">
                <BrandMark size={48} />
              </div>
              <h1>ASTERION TERMINAL</h1>
            </header>
            <div className="setup-steps" aria-live="polite">
              {(runtime?.modules ?? []).map(module => {
                const percent = module.size ? Math.floor((module.done * 100) / module.size) : 0;
                const done = module.state === "done";
                const failedHere = module.state === "failed";
                const installing = module.state === "installing";
                return (
                  <div className="setup-step" key={module.id}>
                    <b>{t(module.title)}</b>
                    <small>{megabytes(module.size)}</small>
                    <div
                      className={`setup-track ${done ? "done" : failedHere ? "failed" : installing ? "indeterminate" : ""}`}
                      role="progressbar"
                      aria-label={t(module.title)}
                      aria-valuemin={0}
                      aria-valuemax={100}
                      aria-valuenow={installing ? undefined : percent}
                    >
                      <i style={installing ? undefined : { width: `${percent}%` }} />
                    </div>
                    <span
                      className={
                        done ? "setup-status-done" : failedHere ? "setup-status-failed" : ""
                      }
                    >
                      {t(
                        {
                          waiting: "等待",
                          downloading: "下载中",
                          downloaded: "已下载",
                          installing: "安装中",
                          done: "完成",
                          failed: "失败",
                        }[module.state] ?? module.state,
                      )}
                      {module.state === "downloading" ? ` ${percent}%` : ""}
                    </span>
                  </div>
                );
              })}
            </div>
            {(error || failed) && (
              <button className="setup-begin primary" onClick={() => void retry()}>
                {t("重试")}
              </button>
            )}
          </section>
        </main>
        <footer className="status-bar">
          <span className="panel-spacer" />
          <NotificationCenter />
        </footer>
      </div>
    </WindowFrame>
  );
}
