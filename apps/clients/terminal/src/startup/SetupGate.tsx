import { Icon } from "../ui/Icon";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useLocale } from "../i18n";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { WindowFrame } from "../host/components/WindowFrame";
import { request, type NativePluginInfo } from "../bridge/client";
import "./setup.css";
import {
  availableResearchPlugins,
  defaultResearchPlugins,
  selectPlugin,
} from "../host/native-plugins";
const marker = "asterion.setup.completed.v1";
function completed() {
  try {
    return localStorage.getItem(marker) === "1";
  } catch {
    return false;
  }
}
export function SetupGate({ children }: { children: ReactNode }) {
  const [returning] = useState(completed);
  const { locale, setLocale } = useLocale();
  const language = locale === "zh-CN" ? "zh" : "en";
  const setLanguage = (value: "zh" | "en") => {
    try {
      setLocale(value === "zh" ? "zh-CN" : "en-US");
    } catch {
      setError(t("保存失败，请检查本机存储权限。"));
    }
  };
  const steps = [t("检查运行环境"), t("初始化本机服务"), t("验证服务连接")];
  const [step, setStep] = useState(0);
  const [running, setRunning] = useState(false);
  const [ready, setReady] = useState(false);
  const [entered, setEntered] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [availablePlugins, setAvailablePlugins] = useState<NativePluginInfo[] | null>(null);
  const [selectedPlugins, setSelectedPlugins] = useState<string[]>([]);
  useEffect(() => {
    if (returning) return;
    let active = true;
    void request("native.plugins.inspect")
      .then(snapshot => {
        if (!active) return;
        const items = availableResearchPlugins(snapshot.native_plugins?.items ?? []);
        setAvailablePlugins(items);
        setSelectedPlugins(defaultResearchPlugins(items));
      })
      .catch(reason => {
        if (active) {
          setError(asDisplayError(reason));
          setAvailablePlugins([]);
        }
      });
    return () => {
      active = false;
    };
  }, [returning]);
  const inFlight = useRef(false);
  async function start() {
    if (inFlight.current) return;
    inFlight.current = true;
    setRunning(true);
    setReady(false);
    setError("");
    setStep(0);
    try {
      const core = await request("runtime.snapshot");
      if (core.protocol !== 1 || core.phase !== "ready") throw new Error(t("本机核心尚未就绪"));
      setStep(1);
      const inspected = await request("node.agent.inspect");
      const program = inspected.agent_program;
      if (!program) throw new Error(t("无法检查本机 Agent 程序"));
      if (program.state === "update_available" || program.state === "recovery_required") {
        if (!program.expected_digest) throw new Error(t("后台组件更新状态异常，请查看详情"));
        await request("node.agent.upgrade", { expected_digest: program.expected_digest });
        const verified = await request("node.agent.inspect");
        if (verified.agent_program?.state !== "current")
          throw new Error(t("后台组件更新尚未完成，请重试启动"));
      }
      const attached = await request("node.local");
      await request("market.local");
      const existingResearch = attached.nodes
        .find(n => n.id === "local")
        ?.health?.services.find(service => service.id === "research");
      if (existingResearch?.desired_running) await request("research.local");
      else if (!existingResearch) {
        const defaults = returning
          ? defaultResearchPlugins(
              (await request("native.plugins.inspect")).native_plugins?.items ?? [],
            )
          : selectedPlugins;
        await request("research.local.create", { plugins: defaults });
      }
      setStep(2);
      const status = await request("runtime.snapshot");
      if (status.stale) throw new Error(t("本机核心尚未就绪"));
      const node = status.nodes.find(n => n.id === "local");
      if (node?.state !== "online" || !node.health?.instance_id)
        throw new Error(t("本机 Agent 尚未就绪，请重试启动"));
      if (!status.market?.transport_online) throw new Error(t("本机行情服务尚未就绪，请重试启动"));
      const expectedResearch = node.health.services.find(service => service.id === "research");
      if (expectedResearch?.desired_running && !status.research?.online)
        throw new Error(t("本机研究服务尚未就绪，请重试启动"));
      setStep(3);
      setReady(true);
      if (returning) setEntered(true);
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      inFlight.current = false;
      setRunning(false);
    }
  }
  useEffect(() => {
    if (returning) void start();
    // start() is guarded by inFlight and must run once per returning launch,
    // not whenever its closure is recreated.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [returning]);
  function enter() {
    try {
      localStorage.setItem(marker, "1");
      setEntered(true);
    } catch {
      setError(t("无法保存首次设置状态，请检查本机存储权限"));
    }
  }
  if (entered) return children;
  return (
    <WindowFrame title={t("Asterion Terminal — 启动设置")}>
      <main className="first-setup" aria-label={t("首次设置与启动")}>
        <div className="setup-language-control">
          <select
            className="setup-language"
            aria-label={t("语言")}
            value={language}
            onChange={e => setLanguage(e.target.value as "zh" | "en")}
          >
            <option value="zh">{t("简体中文")}</option>
            <option value="en">English</option>
          </select>
        </div>
        <section className="setup-content">
          <header>
            <div className="setup-mark">
              <Icon name="asterion" size={48} />
            </div>
            <h1>ASTERION TERMINAL</h1>
            <p>
              {ready
                ? t("工作区已就绪")
                : returning
                  ? t("正在准备你的工作台")
                  : t("准备你的工作区")}
            </p>
          </header>
          <div className="setup-steps" aria-live="polite">
            {steps.map((title, index) => {
              const done = index < step;
              const active = index === step && (running || !!error);
              return (
                <div className="setup-step" key={index}>
                  <b>{title}</b>
                  <div
                    className={`setup-track ${done ? "done" : active ? (error ? "failed" : "indeterminate") : ""}`}
                    role="progressbar"
                    aria-label={title}
                    aria-valuemin={0}
                    aria-valuemax={100}
                    aria-valuenow={done ? 100 : active ? undefined : 0}
                  >
                    <i style={done ? { width: "100%" } : undefined} />
                  </div>
                  <span className={done ? "setup-status-done" : ""}>
                    {done ? t("完成") : active ? (error ? t("失败") : t("进行中")) : t("等待")}
                  </span>
                </div>
              );
            })}
          </div>
          {!returning && !ready && availablePlugins && (
            <fieldset disabled={running} className="setup-plugins">
              <legend>{t("启用的研究插件")}</legend>
              <p>{t("先选择插件，再创建服务。未选中的插件不会部署。")}</p>
              {availablePlugins.map(plugin => (
                <label key={plugin.sha256}>
                  <input
                    type="checkbox"
                    checked={selectedPlugins.includes(plugin.sha256)}
                    onChange={event =>
                      setSelectedPlugins(previous =>
                        selectPlugin(
                          previous,
                          plugin.sha256,
                          event.target.checked,
                          availablePlugins ?? [],
                        ),
                      )
                    }
                  />
                  {plugin.id} · {plugin.version}
                </label>
              ))}
              {availablePlugins.length === 0 && (
                <p>{t("未启用数据源插件；已有历史数据仍保留。")}</p>
              )}
            </fieldset>
          )}
          {error && (
            <p className="setup-error" role="alert">
              <ErrorNotice error={error} namespace="host" />
            </p>
          )}
          <button
            className="setup-begin"
            disabled={running || (!returning && availablePlugins === null)}
            onClick={() => (ready ? enter() : void start())}
          >
            {running
              ? t("正在设置")
              : ready
                ? t("进入工作台")
                : error
                  ? t("重试启动")
                  : t("开始设置")}
          </button>
        </section>
      </main>
    </WindowFrame>
  );
}
