import type { NodeStatus } from "../bridge/client";
import { translate, type MessageValues } from "../i18n";
export const t = (key: string, values?: MessageValues) => translate("host", key, values);
export type Service = NonNullable<NodeStatus["health"]>["services"][number];
export type ServiceKind = Service["kind"];
export const kindLabels: Record<ServiceKind, string> = {
  market: "行情服务",
  data: "历史数据服务",
  research: "研究服务",
  lab: "Python 研究服务",
};
export function serviceStatus(node: NodeStatus, service: Service) {
  if (node.state !== "online") return t("状态未知");
  if (service.state === "stopping") return t("停止中");
  if (service.state === "stopped") return t("已停止");
  if (service.state === "failed") return t("启动失败");
  if (service.state === "starting") return t("启动中");
  return t(
    (
      {
        ready: "业务就绪",
        awaiting_input: service.kind === "market" ? "未登录行情" : "等待初始化",
        degraded: "需要恢复",
        unresponsive: "业务无响应",
        starting: "启动中",
        offline: "离线",
      } as Record<string, string>
    )[service.health] ?? "状态未知",
  );
}
