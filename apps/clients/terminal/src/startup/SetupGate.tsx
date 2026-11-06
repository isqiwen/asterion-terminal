import { BrandMark } from "../ui/BrandMark";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { translate } from "../i18n";
const t = (key: string) => translate("host", key);
import { useLocale } from "../i18n";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { WindowFrame } from "../host/components/WindowFrame";
import { LocalLogsButton } from "../host/components/LocalLogsButton";
import { request, type PythonRuntime } from "../bridge/client";
import { StartupScreen } from "./StartupScreen";
import "./setup.css";

const megabytes = (bytes: number) => `${(bytes / 2 ** 20).toFixed(0)} MB`;

const delay = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));

// What starts behind the startup screen: Lab and the saved Market. Either may
// fail; the status bar and its pages say so once the workbench is open.
async function startUp() {
  const status = await request("runtime.snapshot");
  await Promise.allSettled([
    request("lab.endpoint"),
    status.quotes_endpoint && "host" in status.quotes_endpoint ? request("quotes.open") : null,
  ]);
}

// Every launch shows the startup screen while Lab and Market start; the first
// launch installs the Python runtime first, one row per module.
export function SetupGate({ children }: { children: ReactNode }) {
  const { locale, setLocale } = useLocale();
  const language = locale === "zh-CN" ? "zh" : "en";
  const [runtime, setRuntime] = useState<PythonRuntime | null>(null);
  // The workbench renders under the screen once Lab and Market have started,
  // settles for a moment, then the screen cross-fades into it (startup.css).
  const [phase, setPhase] = useState<"starting" | "beneath" | "leaving" | "entered">("starting");
  const [error, setError] = useState<DisplayError>("");
  const started = useRef(false);
  const enter = () => {
    if (started.current) return;
    started.current = true;
    // The animation reads for at least two seconds; the workbench gets at
    // least 0.6 s beneath it to read its first data.
    const shown = delay(2000);
    void startUp()
      .then(() => {
        setPhase("beneath");
        return Promise.all([shown, delay(600)]);
      })
      .then(() => {
        setPhase("leaving");
        return delay(900);
      })
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
  const failed = runtime?.state === "failed";
  return (
    <WindowFrame title={t("Asterion Terminal — 首次启动")}>
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
                    className={done ? "setup-status-done" : failedHere ? "setup-status-failed" : ""}
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
            <p className="setup-error" role="alert">
              {failed ? runtime?.error : <ErrorNotice error={error} namespace="host" />}{" "}
              <LocalLogsButton />
            </p>
          )}
          {(error || failed) && (
            <button className="setup-begin primary" onClick={() => void retry()}>
              {t("重试")}
            </button>
          )}
        </section>
      </main>
    </WindowFrame>
  );
}
