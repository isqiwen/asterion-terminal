import { useState } from "react";
import type { CommandCall, CommandHandler } from "../bridge/commands";
import type { Snapshot } from "../bridge/client";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { ServiceActionDialog } from "./ServiceActionDialog";
import { binding, kindLabels, serviceStatus, t, type Service } from "./service-state";
type Action = "stop" | "restart";
type Pending = { service: string; revision: string; action: Action };
const actionLabels: Record<Action, string> = { stop: "停止服务", restart: "重启服务" };
const active = ["queued", "running", "cancel_requested", "publishing"];
// The services the Terminal runs on this machine. They start with it and exit
// with it; a stopped service stays stopped until started here.
export function Connections({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const [error, setError] = useState<DisplayError>("");
  const [pending, setPending] = useState<Pending | null>(null);
  const node = snapshot?.nodes.find(n => n.id === "local");
  const services = node?.health?.services ?? [];
  async function run(...args: CommandCall) {
    setError("");
    try {
      await trade(...args);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  }
  const target = services.find(s => s.id === pending?.service);
  const changed = !target || target.revision !== pending?.revision;
  const tasks = (service: Service | undefined) =>
    service?.kind === "data" || service?.kind === "research"
      ? ((service.kind === "data" ? snapshot?.data : snapshot?.research_service)?.tasks.filter(
          task => active.includes(task.state),
        ) ?? [])
      : [];
  const open = tasks(target);
  return (
    <section className="service-connections" aria-label={t("本机服务")}>
      <p className="subtle">
        {t("本机服务由 Terminal 启动，随 Terminal 退出。停止的服务保持停止，直到在此启动。")}
      </p>
      {error && (
        <p role="alert">
          <ErrorNotice error={error} namespace="host" />
        </p>
      )}
      <div className="managed-services" role="list" aria-label={t("服务列表")}>
        {services.map(service => (
          <section
            className="managed-service"
            role="listitem"
            aria-label={service.id}
            key={service.id}
          >
            <div className="deployment-heading">
              <div>
                <strong>{t(kindLabels[service.kind])}</strong>
                <p className="subtle">
                  {service.id} · {node && serviceStatus(node, service)}
                  {binding(snapshot, service.kind)?.service === service.id && " · " + t("当前使用")}
                </p>
              </div>
              <div className="managed-service-actions">
                {service.desired_running ? (
                  <>
                    <button
                      disabled={busy}
                      onClick={() =>
                        setPending({
                          service: service.id,
                          revision: service.revision,
                          action: "stop",
                        })
                      }
                    >
                      {t("停止")}
                    </button>
                    <button
                      disabled={busy || service.state !== "running"}
                      onClick={() =>
                        setPending({
                          service: service.id,
                          revision: service.revision,
                          action: "restart",
                        })
                      }
                    >
                      {t("重启")}
                    </button>
                  </>
                ) : (
                  <button
                    disabled={busy}
                    onClick={() =>
                      void run("node.action", { service: service.id, action: "start" })
                    }
                  >
                    {t("启动")}
                  </button>
                )}
              </div>
            </div>
            <details>
              <summary>{t("技术详情")}</summary>
              <p>
                PID {service.pid} · {service.endpoint}
              </p>
              <p>
                {t("自动重启")} {service.restarts}/3
              </p>
              <p>{service.directory}</p>
              <p>{service.error}</p>
            </details>
          </section>
        ))}
        {!services.length && (
          <p className="subtle">{t("尚无本机服务；使用相应功能时自动启动。")}</p>
        )}
      </div>
      {pending && (
        <ServiceActionDialog
          title={t(actionLabels[pending.action])}
          busy={busy}
          disabled={busy || changed}
          onCancel={() => {
            setPending(null);
            setError("");
          }}
          onConfirm={() =>
            void (async () => {
              if (
                !changed &&
                (await run("node.action", { service: pending.service, action: pending.action }))
              )
                setPending(null);
            })()
          }
        >
          <p>
            <strong>{pending.service}</strong>
          </p>
          {changed ? (
            <p role="alert">{t("目标状态已变化，请取消后重新检查。")}</p>
          ) : (
            <>
              <p>{t("此服务的连接会中断，运行中的任务可能中断。")}</p>
              {open.length > 0 && (
                <p>
                  {t("当前可见的未完成任务：{tasks}", {
                    tasks: open.map(task => task.id).join(", "),
                  })}
                </p>
              )}
            </>
          )}
          {error && (
            <p role="alert">
              <ErrorNotice error={error} namespace="host" />
            </p>
          )}
        </ServiceActionDialog>
      )}
    </section>
  );
}
