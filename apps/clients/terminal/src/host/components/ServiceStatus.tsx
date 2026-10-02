import { useEffect, useRef, useState } from "react";
import type { Snapshot } from "../../bridge/client";
import { translate, useLocale } from "../../i18n";
const t = (key: string) => translate("host", key);
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
  const uncertain = failed || !snapshot || !!snapshot.stale;
  const unhealthy = snapshot?.nodes.some(
    n =>
      n.state !== "online" ||
      !n.health ||
      n.health.services.some(
        s =>
          s.desired_running &&
          (s.state !== "running" || !!s.error || !["ready", "awaiting_input"].includes(s.health)),
      ),
  );
  const kinds: Record<string, string> = {
    market: "实时行情",
    research: "回测与因子研究",
    paper: "模拟交易",
    live: "CTP 交易",
    strategy: "策略运行",
  };
  const health: Record<string, string> = {
    ready: "正常",
    awaiting_input: "等待配置",
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
        className={uncertain || unhealthy ? "bad" : "good"}
        aria-label={t("查看服务连接")}
        aria-expanded={open}
        aria-controls="service-status-panel"
        onClick={() => setOpen(!open)}
      >
        ● {t(uncertain ? "服务待确认" : unhealthy ? "服务异常" : "服务")}
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
                <span>{t(uncertain || node.state !== "online" ? "待确认" : "已连接")}</span>
              </div>
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
              {!!node.health?.services.length && (
                <details className="service-connection-details">
                  <summary>{t("连接详情")}</summary>
                  {node.health.services.map(service => (
                    <div className="service-status-row" key={service.id}>
                      <span>{t(kinds[service.kind] ?? "服务")}</span>
                      <code>{service.id}</code>
                    </div>
                  ))}
                </details>
              )}
            </div>
          ))}
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
