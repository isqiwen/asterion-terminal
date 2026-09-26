import { useEffect, useState } from "react";
import { apiBase, request } from "@asterion/runtime-client/client";
import type { components } from "@asterion/api-types/schema";
import { openSettings } from "../settings/window";

type Report = components["schemas"]["ServiceReport"];
const labels = {
  ready: "正常",
  disabled: "已停用",
  unavailable: "不可用",
  unknown: "无法确认",
  unconfigured: "未配置",
  configured: "已配置 · 按需连接",
  not_integrated: "尚未接入",
};
export function ServiceStatus({
  token,
  connected,
}: {
  token: string;
  connected: boolean;
}) {
  const [open, setOpen] = useState(false);
  const [report, setReport] = useState<Report>();
  const [error, setError] = useState("");
  const [refresh, setRefresh] = useState(0);
  useEffect(() => {
    if (!open) return;
    let active = true;
    let timer: ReturnType<typeof setTimeout>;
    setReport(undefined);
    setError("");
    const check = async () => {
      try {
        const next = await request<Report>("/services", token, undefined, 5000);
        if (active) {
          setReport(next);
          setError("");
        }
      } catch (e) {
        if (active) {
          setReport(undefined);
          setError(String(e));
        }
      } finally {
        if (active) timer = setTimeout(check, 5000);
      }
    };
    void check();
    return () => {
      active = false;
      clearTimeout(timer);
    };
  }, [open, token, refresh]);
  useEffect(() => {
    if (!open) return;
    const close = (event: KeyboardEvent) => {
      if (event.key === "Escape") setOpen(false);
    };
    window.addEventListener("keydown", close);
    return () => window.removeEventListener("keydown", close);
  }, [open]);
  return (
    <div className="service-status">
      <button
        className={connected ? "good" : "muted"}
        title="查看服务状态"
        aria-label={`查看服务状态（本机 API${connected ? "可达" : "未就绪"}）`}
        aria-expanded={open}
        aria-controls="service-status-panel"
        onClick={() => setOpen(!open)}
      >
        ● 服务
      </button>
      {open && (
        <section
          id="service-status-panel"
          className="service-status-panel"
          aria-label="服务连接详情"
        >
          <header>
            <strong>服务连接</strong>
            <button onClick={() => setOpen(false)} aria-label="关闭服务详情">
              ×
            </button>
          </header>
          <p className="muted">{apiBase()} · 每 5 秒更新</p>
          {error && (
            <div role="alert">
              无法获取当前服务状态，其他服务状态无法确认。<p>{error}</p>
            </div>
          )}
          {!report && !error && <p>正在检测本机服务…</p>}
          {report?.services.map((service) => (
            <div className="service-status-row" key={service.id}>
              <div>
                <strong>{service.name}</strong>
                <span
                  className={
                    service.state === "ready"
                      ? "good"
                      : service.state === "unavailable"
                        ? "service-failed"
                        : "muted"
                  }
                >
                  {labels[service.state]}
                </span>
              </div>
              <p>{service.detail}</p>
            </div>
          ))}
          <footer>
            <span className="muted">
              {report
                ? `检查于 ${new Date(report.checked_at * 1000).toLocaleTimeString()}`
                : "尚无有效检测结果"}
            </span>
            <button onClick={() => setRefresh((n) => n + 1)}>重新检测</button>
            <button
              onClick={() =>
                void openSettings("数据源").catch((e) => setError(String(e)))
              }
            >
              数据源设置
            </button>
          </footer>
        </section>
      )}
    </div>
  );
}
