import type { CommandCall } from "../bridge/commands";
import type { NodeStatus, Snapshot } from "../bridge/client";
import { binding, kindLabels, matches, serviceStatus, t, type Service } from "./service-state";

export type ServiceAction = "stop" | "restart" | "use" | "update";

// One service on a node: what applies to it now, and its management details.
// Actions that interrupt it are proposed for confirmation, not run.
export function ManagedService({
  snapshot,
  node,
  service,
  disabled,
  run,
  propose,
}: {
  snapshot: Snapshot | null;
  node: NodeStatus;
  service: Service;
  disabled: boolean;
  run: (...args: CommandCall) => Promise<unknown>;
  propose: (action: ServiceAction, target: NodeStatus, service: Service) => void;
}) {
  const active = matches(binding(snapshot, service.kind), node, service);
  const unavailable = disabled || node.state !== "online" || !!node.health?.maintenance;
  return (
    <section className="managed-service" role="listitem" aria-label={service.id}>
      <div className="deployment-heading">
        <div>
          <strong>{service.id}</strong>
          <p className="subtle">
            {t(kindLabels[service.kind])} · {serviceStatus(node, service)}
          </p>
        </div>
        {/* Only what applies now: a stopped service starts, a wanted
            one stops, a running one restarts. */}
        <div className="managed-service-actions">
          {!service.desired_running && (
            <button
              disabled={unavailable || service.state === "running" || service.state === "starting"}
              onClick={() =>
                void run("node.action", {
                  id: node.id,
                  service: service.id,
                  action: "start",
                })
              }
            >
              {t("启动")}
            </button>
          )}
          {service.desired_running && (
            <button
              disabled={unavailable || !service.desired_running}
              onClick={() => propose("stop", node, service)}
            >
              {t("停止")}
            </button>
          )}
          {service.state === "running" && (
            <button
              disabled={unavailable || service.state !== "running"}
              onClick={() => propose("restart", node, service)}
            >
              {t("重启")}
            </button>
          )}
          {active ? (
            <span className="deployment-badge">{t("当前使用")}</span>
          ) : (
            <button
              disabled={unavailable || service.state !== "running"}
              onClick={() => propose("use", node, service)}
            >
              {t("使用此服务")}
            </button>
          )}
        </div>
      </div>
      <details>
        <summary>{t("管理服务")}</summary>
        <details>
          <summary>{t("程序更新")}</summary>
          <p>
            {t("先停止服务（会中断连接与任务），再更新程序。保留配置与数据，更新后需手动启动。")}
          </p>
          <button
            disabled={
              unavailable ||
              service.state !== "stopped" ||
              service.desired_running ||
              !service.revision
            }
            onClick={() => propose("update", node, service)}
          >
            {t("更新已停止的服务")}
          </button>
        </details>
        {node.id !== "local" && (
          <details>
            <summary>{t("端口访问")}</summary>
            <div className="source-actions">
              <button
                disabled={unavailable}
                onClick={() =>
                  void run("node.service_firewall", {
                    id: node.id,
                    service: service.id,
                    action: "allow",
                    token: "",
                  })
                }
              >
                {t("检查端口放行")}
              </button>
              <button
                disabled={unavailable}
                onClick={() =>
                  void run("node.service_firewall", {
                    id: node.id,
                    service: service.id,
                    action: "remove",
                    token: "",
                  })
                }
              >
                {t("检查规则撤销")}
              </button>
            </div>
          </details>
        )}
        <details>
          <summary>{t("技术详情")}</summary>
          <p>
            PID {service.pid} · {service.port || service.endpoint}
          </p>
          <p>
            {t("自动重启")} {service.restarts}/3
          </p>
          <p>{service.directory}</p>
          <p>{service.error}</p>
        </details>
      </details>
    </section>
  );
}
