import { Icon } from "../ui/Icon";
import { BackendError, ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useLocale } from "../i18n";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { WindowFrame } from "../host/components/WindowFrame";
import { readableCtpConnection, request } from "../bridge/client";
import "./setup.css";
import { defaultDataTaskPlugins } from "../host/native-plugins";
export function SetupGate({ children }: { children: ReactNode }) {
  const { locale, setLocale } = useLocale();
  const language = locale === "zh-CN" ? "zh" : "en";
  const setLanguage = (value: "zh" | "en") => {
    try {
      setLocale(value === "zh" ? "zh-CN" : "en-US");
    } catch {
      setError(t("保存失败，请检查本机存储权限。"));
    }
  };
  const steps = [t("服务管理器"), t("行情服务"), t("数据服务"), t("任务服务"), t("交易服务")];
  // Things the user should know before entering; none of them blocks entry,
  // but the screen waits for the user while there are any.
  const [notices, setNotices] = useState<string[]>([]);
  const [step, setStep] = useState(0);
  const [running, setRunning] = useState(false);
  const [ready, setReady] = useState(false);
  const [entered, setEntered] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const inFlight = useRef(false);
  async function start() {
    if (inFlight.current) return;
    inFlight.current = true;
    setRunning(true);
    setReady(false);
    setError("");
    setStep(0);
    try {
      setNotices([]);
      const core = await request("runtime.snapshot");
      if (core.protocol !== 1 || core.phase !== "ready") throw new Error(t("本机核心尚未就绪"));
      const inspected = await request("node.agent.inspect");
      const program = inspected.agent_program;
      if (!program) throw new Error(t("无法检查本机服务管理器 程序"));
      if (program.state === "update_available" || program.state === "recovery_required") {
        if (!program.expected_digest) throw new Error(t("后台组件更新状态异常，请查看详情"));
        await request("node.agent.upgrade", { expected_digest: program.expected_digest });
        const verified = await request("node.agent.inspect");
        if (verified.agent_program?.state !== "current")
          throw new Error(t("后台组件更新尚未完成，请重试启动"));
      }
      await request("node.local");
      for (const deadline = Date.now() + 20000; ;) {
        const status = await request("runtime.snapshot");
        const node = status.nodes.find(n => n.id === "local");
        if (node?.health?.phase === "recovery_required") {
          const failure = node.health.failure;
          throw new BackendError(
            "recovery_required",
            failure?.message ?? "Agent recovery is required",
          );
        }
        if (node?.state === "online" && node.health?.phase === "ready") break;
        if (Date.now() >= deadline) throw new Error(t("本机服务管理器尚未就绪，请重试启动"));
        await new Promise(resolve => setTimeout(resolve, 300));
      }
      setStep(1);
      // A step completes only when its service has answered its first
      // heartbeat, so the steps move forward only and the workbench opens
      // with everything already healthy.
      const healthy = ["ready", "awaiting_input"];
      const settled = async (kind: "market" | "data" | "task", failure: string) => {
        for (const deadline = Date.now() + 20000; ;) {
          const status = await request("runtime.snapshot");
          const node = status.nodes.find(n => n.id === "local");
          if (node?.state !== "online" || !node.health?.instance_id) {
            setStep(0);
            throw new Error(t("本机服务管理器尚未就绪，请重试启动"));
          }
          const selected =
            kind === "market"
              ? status.market?.service
              : kind === "data"
                ? status.data?.service
                : status.task_service?.service;
          const expected = node.health.services.find(service => service.id === selected);
          const online =
            kind === "market"
              ? !!status.market?.transport_online
              : kind === "data"
                ? !!status.data?.online
                : !!status.task_service?.online;
          if (expected?.state === "failed") throw new Error(t(failure));
          if (
            node.health.phase === "ready" &&
            online &&
            expected?.desired_running &&
            healthy.includes(expected.health)
          )
            return status;
          if (Date.now() >= deadline) throw new Error(t(failure));
          await new Promise(resolve => setTimeout(resolve, 300));
        }
      };
      await request("market.local");
      const marketReady = await settled("market", "本机行情服务尚未就绪，请重试启动");
      setStep(2);
      // Initial Agent attachment may precede recovery of its service inventory.
      const existingTask = marketReady.nodes
        .find(n => n.id === "local")
        ?.health?.services.find(service => service.id === "task");
      const plugins = (await request("native.plugins.inspect")).native_plugins?.items ?? [];
      // A service left stopped is started again: the workbench never opens
      // without it.
      if (existingTask) await request("node.data_tasks.local.open");
      else
        await request("node.data_tasks.local.create", { plugins: defaultDataTaskPlugins(plugins) });
      await settled("data", "本机数据服务尚未就绪，请重试启动");
      setStep(3);
      const status = await settled("task", "本机任务服务尚未就绪，请重试启动");
      // Market data must still be reachable once everything is up.
      if (!status.market?.transport_online) {
        setStep(1);
        throw new Error(t("本机行情服务尚未就绪，请重试启动"));
      }
      const invalid = plugins.filter(plugin => plugin.state === "invalid");
      const interrupted = status.task_service?.interrupted_count ?? 0;
      setStep(4);
      // Each account that trades has its own service. Starting it only runs
      // the program: nothing logs in or reaches the counter. An account
      // whose service does not come up is reported and can be started from
      // the Trading page; it does not keep the workbench closed.
      const unopened: string[] = [];
      for (const account of (status.ctp_connections ?? []).filter(readableCtpConnection)) {
        if (!account.trading_record) continue;
        try {
          await request("live.open", { account: account.id });
        } catch {
          unopened.push(account.name);
        }
      }
      let trading = true;
      for (const deadline = Date.now() + 20000; ;) {
        const services =
          (await request("runtime.snapshot")).nodes
            .find(n => n.id === "local")
            ?.health?.services.filter(
              service => service.kind === "live" && service.desired_running,
            ) ?? [];
        if (services.every(service => healthy.includes(service.health))) break;
        if (Date.now() >= deadline) {
          trading = false;
          break;
        }
        await new Promise(resolve => setTimeout(resolve, 300));
      }
      const pending = [
        ...unopened.map(name =>
          t("账户 {p0} 的交易服务未能启动，可在交易页查看原因并重试", { p0: name }),
        ),
        ...(trading ? [] : [t("部分交易服务尚未就绪，可在状态栏查看")]),
        ...invalid.map(plugin =>
          t("插件 {p0} 无法加载，可在设置的插件页查看原因", { p0: plugin.id || plugin.file }),
        ),
        ...(interrupted ? [t("{p0} 项任务上次被中断，可在任务中心重试", { p0: interrupted })] : []),
      ];
      setNotices(pending);
      setStep(5);
      setReady(true);
      // With nothing to point out, the workbench opens by itself; otherwise
      // the notices stay up until the user enters.
      if (!pending.length) setEntered(true);
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      inFlight.current = false;
      setRunning(false);
    }
  }
  useEffect(() => {
    void start();
    // start() is guarded by inFlight and must run once per launch, not
    // whenever its closure is recreated.
  }, []);
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
            <p>{ready ? t("工作区已就绪") : t("正在准备你的工作台")}</p>
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
          {ready && notices.length > 0 && (
            <ul className="setup-notices" aria-label={t("进入前请留意")}>
              {notices.map(notice => (
                <li key={notice}>{notice}</li>
              ))}
            </ul>
          )}
          {error && (
            <p className="setup-error" role="alert">
              <ErrorNotice error={error} namespace="host" />
            </p>
          )}
          <button
            className="setup-begin primary"
            disabled={running}
            onClick={() => (ready ? setEntered(true) : void start())}
          >
            {running ? t("正在准备") : ready ? t("进入工作台") : t("重试启动")}
          </button>
        </section>
      </main>
    </WindowFrame>
  );
}
