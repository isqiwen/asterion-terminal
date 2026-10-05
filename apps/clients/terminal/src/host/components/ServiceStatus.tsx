import { useEffect, useRef, useState } from "react";
import type { ExecutionHealth, Snapshot } from "../../bridge/client";
import { translate, useLocale } from "../../i18n";
import { BackendError, ErrorNotice } from "../../i18n/errors";
const t = (key: string) => translate("host", key);
const progressLabels: Record<Exclude<keyof ExecutionHealth, "business_ready">, string> = {
  io: "请求处理",
  state: "状态推进",
  persistence: "持久化与准备",
  initialization: "初始化",
  command: "当前命令",
};
function ExecutionStatus({
  execution,
  stale,
  elapsed,
}: {
  execution: ExecutionHealth | null;
  stale: boolean;
  elapsed: number;
}) {
  if (!execution) return <p>{t("尚无线程进展观测")}</p>;
  const readinessStale =
    stale || execution.state.age_ms + elapsed > 30000 || execution.io.age_ms + elapsed > 30000;
  return (
    <>
      <div className="service-status-row">
        <span>{t("业务就绪")}</span>
        <span>{t(readinessStale ? "待确认" : execution.business_ready ? "已就绪" : "未就绪")}</span>
      </div>
      {Object.entries(progressLabels).map(([key, label]) => {
        const progress = execution[key as keyof typeof progressLabels];
        const age = progress.age_ms + elapsed;
        const periodic = key === "io" || key === "state";
        return (
          <div className="service-status-row" key={key}>
            <span>{t(label)}</span>
            <span>
              {t(
                stale
                  ? "待确认"
                  : !progress.observed
                    ? "尚未开始"
                    : age > 30000 && (periodic || progress.pending)
                      ? "进展超时"
                      : progress.pending
                        ? "处理中"
                        : periodic
                          ? "正常"
                          : "空闲",
              )}
              {!stale && progress.observed && (
                <>
                  {" "}
                  · {Math.floor(age / 1000)} {t("秒")}
                </>
              )}
            </span>
          </div>
        );
      })}
    </>
  );
}
export function ServiceStatus({
  snapshot,
  failed,
  busy,
  checkedAt,
  refresh,
  settings,
}: {
  snapshot: Snapshot | null;
  failed: boolean;
  busy: boolean;
  checkedAt: number | null;
  refresh: () => void;
  settings: () => void;
}) {
  const { locale } = useLocale();
  const [open, setOpen] = useState(false);
  const root = useRef<HTMLDivElement>(null);
  const trigger = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (!open) return;
    const outside = (e: PointerEvent) => {
      if (!root.current?.contains(e.target as Node)) setOpen(false);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        setOpen(false);
        trigger.current?.focus();
      }
    };
    document.addEventListener("pointerdown", outside);
    document.addEventListener("keydown", key);
    return () => {
      document.removeEventListener("pointerdown", outside);
      document.removeEventListener("keydown", key);
    };
  }, [open]);
  const uncertain = failed || !snapshot || !!snapshot.diagnostics.refresh_failed;
  const loggingFailed = !!snapshot?.diagnostics.log_failures;
  // A service that was just started has not answered its first heartbeat
  // yet (the missed probe is its reported error); that is not a fault and is
  // shown as starting, not as an error.
  const starting = (s: { state: string; health: string }) =>
    ["running", "starting"].includes(s.state) && s.health === "starting";
  const services = snapshot?.nodes.flatMap(n => n.health?.services ?? []) ?? [];
  const unhealthy = snapshot?.nodes.some(
    n =>
      n.state !== "online" ||
      !n.health ||
      n.health.phase === "recovery_required" ||
      n.health.services.some(
        s =>
          s.desired_running &&
          !starting(s) &&
          (s.state !== "running" || !!s.error || !["ready", "awaiting_input"].includes(s.health)),
      ),
  );
  const warming =
    !unhealthy &&
    (snapshot?.nodes.some(n => n.health?.phase === "initializing") ||
      services.some(s => s.desired_running && starting(s)));
  const kinds: Record<string, string> = {
    market: "行情服务",
    task: "任务服务",
    data: "数据服务",
    live: "交易服务",
  };
  const health: Record<string, string> = {
    ready: "正常",
    awaiting_input: "等待配置或连接",
    degraded: "异常",
    unavailable: "不可用",
    stopped: "已停止",
    starting: "启动中",
    unknown: "待确认",
  };
  return (
    <div className="service-status" ref={root}>
      <button
        ref={trigger}
        className={uncertain || unhealthy || loggingFailed ? "bad" : warming ? undefined : "good"}
        aria-label={t("查看服务连接")}
        aria-expanded={open}
        aria-controls="service-status-panel"
        onClick={() => setOpen(!open)}
      >
        ●{" "}
        {t(
          uncertain
            ? "服务待确认"
            : unhealthy
              ? "服务异常"
              : loggingFailed
                ? "诊断不完整"
                : warming
                  ? "服务启动中"
                  : "服务",
        )}
      </button>
      {open && (
        <section
          id="service-status-panel"
          className="service-status-panel"
          aria-label={t("服务连接详情")}
        >
          <header>
            <strong>{t("服务连接")}</strong>
            <button
              aria-label={t("关闭服务详情")}
              onClick={() => {
                setOpen(false);
                trigger.current?.focus();
              }}
            >
              ×
            </button>
          </header>
          {snapshot?.nodes.map(node => (
            <div className="service-status-node" key={node.id}>
              <div className="service-status-row">
                <strong>{node.id === "local" ? t("此电脑") : node.id}</strong>
                <span>
                  {t(
                    uncertain || node.state !== "online"
                      ? "待确认"
                      : node.health?.phase === "initializing"
                        ? "初始化"
                        : node.health?.phase === "recovery_required"
                          ? "需要恢复后继续"
                          : "已连接",
                  )}
                </span>
              </div>
              {node.health?.failure && (
                <p role="status">
                  <ErrorNotice
                    error={new BackendError(node.health.failure.code, node.health.failure.message)}
                  />
                </p>
              )}
              {node.health?.services.map(service => (
                <div className="service-status-row" key={service.id}>
                  <span>{t(kinds[service.kind] ?? "服务")}</span>
                  <span>
                    {t(
                      uncertain || node.state !== "online"
                        ? "待确认"
                        : !service.desired_running && !service.pid
                          ? "已停止"
                          : (health[service.health] ?? "待确认"),
                    )}
                  </span>
                </div>
              ))}
              {node.health && (
                <details className="service-connection-details">
                  <summary>{t("连接详情")}</summary>
                  <div className="service-status-row">
                    <span>Agent</span>
                  </div>
                  <ExecutionStatus
                    execution={node.health.execution}
                    stale={uncertain || node.state !== "online"}
                    elapsed={Math.max(0, Date.now() - node.last_heartbeat_ms)}
                  />
                  {node.health.services.map(service => (
                    <div key={service.id}>
                      <div className="service-status-row">
                        <span>{t(kinds[service.kind] ?? "服务")}</span>
                        <code>{service.id}</code>
                      </div>
                      <ExecutionStatus
                        execution={service.execution}
                        stale={
                          uncertain ||
                          node.state !== "online" ||
                          service.state !== "running" ||
                          service.health === "unresponsive"
                        }
                        elapsed={Math.max(0, Date.now() - node.last_heartbeat_ms)}
                      />
                    </div>
                  ))}
                </details>
              )}
            </div>
          ))}
          {loggingFailed && (
            <p role="status">
              {t("本次运行有日志写入失败，诊断记录可能不完整。请检查日志目录的空间和权限。")}
            </p>
          )}
          {!!snapshot?.diagnostics.refresh_failed && (
            <p role="status">{t("后台状态刷新失败，当前显示为最近成功读取的状态。")}</p>
          )}
          {uncertain && <p role="status">{t("无法确认当前服务状态")}</p>}
          <footer>
            <span>
              {checkedAt ? new Date(checkedAt).toLocaleTimeString(locale) : t("尚未检查")}
            </span>
            <button disabled={busy} onClick={refresh}>
              {t("重新检测")}
            </button>
            <button
              onClick={() => {
                setOpen(false);
                settings();
              }}
            >
              {t("连接设置")}
            </button>
          </footer>
        </section>
      )}
    </div>
  );
}
