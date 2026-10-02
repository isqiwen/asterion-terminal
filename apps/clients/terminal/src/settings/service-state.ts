import type { NodeStatus, Snapshot } from "../bridge/client";
import { translate, type MessageValues } from "../i18n";
export const t = (key: string, values?: MessageValues) => translate("host", key, values);
export type Service = NonNullable<NodeStatus["health"]>["services"][number];
export type ServiceKind = Service["kind"];
export const kindLabels: Record<ServiceKind, string> = {
  market: "实时行情",
  research: "研究与计算",
  live: "CTP 交易",
};
export type Binding = {
  service: string;
  remote: boolean;
  host: string;
  port: number;
  online: boolean;
};
export function binding(snapshot: Snapshot | null, kind: ServiceKind): Binding | null {
  if (!snapshot) return null;
  if (kind === "market") {
    const value = snapshot.market;
    return value && { ...value, online: value.transport_online };
  }
  if (kind === "research") return snapshot.research;
  const value = snapshot.live?.connection;
  return value
    ? {
        service: value.session ?? "",
        remote: value.transport === "tcp_tls",
        host: value.host ?? "localhost",
        port: value.port ?? 0,
        online: value.state === "connected",
      }
    : null;
}
export function matches(active: Binding | null, node: NodeStatus, service: Service) {
  return (
    !!active &&
    active.service === service.id &&
    (active.remote
      ? node.id !== "local" && active.host === node.host && active.port === service.port
      : node.id === "local")
  );
}
export function location(active: Binding | null, nodes: NodeStatus[]) {
  if (!active) return t("本机（默认）");
  if (!active.remote) return t("本机");
  const node = nodes.find(n => n.health?.services.some(s => matches(active, n, s)));
  return node?.id ?? `${active.host}:${active.port}`;
}
export function serviceStatus(node: NodeStatus, service: Service) {
  if (node.state !== "online") return t("状态未知");
  if (service.state === "stopped") return t("已停止");
  if (service.state === "failed") return t("启动失败");
  return t(
    (
      {
        ready: "业务就绪",
        awaiting_input: "等待初始化",
        degraded: "需要恢复",
        unresponsive: "业务无响应",
        starting: "启动中",
        offline: "离线",
      } as Record<string, string>
    )[service.health] ?? "状态未知",
  );
}

export function runtimeStatus(snapshot: Snapshot | null, kind: ServiceKind) {
  const active = binding(snapshot, kind);
  if (!active) return t("按需启动");
  if (!active.online) return t("失联 · 状态未知");
  if (kind === "market")
    return t(
      snapshot?.market?.phase === "connected"
        ? "行情已连接"
        : snapshot?.market?.phase === "error"
          ? "行情连接失败"
          : "等待行情登录",
    );
  if (kind === "live")
    return t(snapshot?.live?.session?.phase === "ready" ? "账户已就绪" : "等待账户连接");
  return t("已连接");
}
