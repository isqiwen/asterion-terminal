import { BrandMark } from "../ui/BrandMark";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useLocale } from "../i18n";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { WindowFrame } from "../host/components/WindowFrame";
import { LocalLogsButton } from "../host/components/LocalLogsButton";
import { request, type PythonRuntime } from "../bridge/client";
import { StartupScreen } from "./StartupScreen";
import "./setup.css";

const megabytes = (bytes: number) => `${(bytes / 2 ** 20).toFixed(0)} MB`;

// What starts behind the startup screen: Lab and the saved Market. Either may
// fail; the status bar and its pages say so once the workbench is open. The
// screen stays long enough for its animation to read.
async function startUp() {
  const status = await request("runtime.snapshot");
  await Promise.allSettled([
    request("lab.endpoint"),
    status.market_endpoint && "host" in status.market_endpoint ? request("market.open") : null,
    new Promise(resolve => setTimeout(resolve, 2000)),
  ]);
}

// Every launch shows the startup screen while Lab and Market start; the first
// launch installs the Python runtime first, one row per module.
export function SetupGate({ children }: { children: ReactNode }) {
  const { locale, setLocale } = useLocale();
  const language = locale === "zh-CN" ? "zh" : "en";
  const [runtime, setRuntime] = useState<PythonRuntime | null>(null);
  const [entered, setEntered] = useState(false);
  const [leaving, setLeaving] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const started = useRef(false);
  const enter = () => {
    if (started.current) return;
    started.current = true;
    void startUp()
      .then(() => {
        setLeaving(true);
        // The screen's fade-out, startup.css .leaving.
        return new Promise(resolve => setTimeout(resolve, 380));
      })
      .then(() => setEntered(true));
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
  if (entered) return children;
  if ((!runtime || runtime.state === "ready") && !error) return <StartupScreen leaving={leaving} />;
  const failed = runtime?.state === "failed";
  const total = (runtime?.modules ?? []).reduce((sum, module) => sum + module.size, 0);
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
            <p>
              {t(
                "第一次启动：下载并安装研究所需的 Python 运行时，共 {size}。以后启动直接进入工作台。",
                {
                  size: megabytes(total),
                },
              )}
            </p>
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
