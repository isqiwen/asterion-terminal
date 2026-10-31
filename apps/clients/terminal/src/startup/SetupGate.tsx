import { BrandMark } from "../ui/BrandMark";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useLocale } from "../i18n";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { WindowFrame } from "../host/components/WindowFrame";
import { LocalLogsButton } from "../host/components/LocalLogsButton";
import { request, type PythonRuntime } from "../bridge/client";
import "./setup.css";

const megabytes = (bytes: number) => `${(bytes / 2 ** 20).toFixed(0)} MB`;

// What the Terminal does in the background once the workbench is open: the
// local market service, the services a previous session ran, and the saved
// trading accounts. Where one does not come up, its own page says so.
async function restore() {
  const status = await request("runtime.snapshot");
  const services = status.nodes.find(node => node.id === "local")?.health?.services ?? [];
  const market = services.find(item => item.id === "market-data" && item.kind === "market");
  const work: Promise<unknown>[] = [];
  if (!(market?.state === "stopped" && !market.desired_running)) work.push(request("market.local"));
  if (services.some(item => item.kind === "data" && item.desired_running))
    work.push(request("node.data.local.open"));
  if (services.some(item => item.kind === "research" && item.desired_running))
    work.push(request("node.research.local.open"));
  for (const account of (status.trading_connections ?? []).filter(item => !("error" in item)))
    work.push(request("live.open", { account: account.id }));
  await Promise.allSettled(work);
}

// The first launch installs the Python runtime, one row per module; every
// later launch opens the workbench at once.
export function SetupGate({ children }: { children: ReactNode }) {
  const { locale, setLocale } = useLocale();
  const language = locale === "zh-CN" ? "zh" : "en";
  const [runtime, setRuntime] = useState<PythonRuntime | null>(null);
  const [entered, setEntered] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const restored = useRef(false);
  const enter = () => {
    setEntered(true);
    if (restored.current) return;
    restored.current = true;
    void restore();
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
  // Nothing to show until the Terminal says whether a runtime is installed.
  if (!runtime && !error) return null;
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
                "第一次启动：下载并安装研究与数据所需的 Python 运行时，共 {size}。以后启动直接进入工作台。",
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
