import type { FirewallPlan, TerminalCommand } from "../bridge/client";
import { t } from "./service-state";
export function FirewallPreview({
  plan,
  busy,
  keyReady = false,
  privateKey = "",
  clearKey = () => {},
  run,
}: {
  plan: FirewallPlan;
  busy: boolean;
  keyReady?: boolean;
  privateKey?: string;
  clearKey?: () => void;
  run: (method: TerminalCommand, params: Record<string, string>) => Promise<boolean>;
}) {
  return (
    <section aria-label={t("防火墙规则预览")}>
      <h3>
        {plan.action === "remove" ? t("撤销规则") : t("放行规则")} · {plan.id}
      </h3>
      <p>
        {t("目标")}
        {plan.host} · TCP {plan.port}
        {t("· 允许来源")}
        {plan.source}
        {t("（单台主机）")}
      </p>
      <p>
        {plan.backend} ·{" "}
        {{
          active: t("防火墙已启用"),
          read_only: t("防火墙已启用，专用账户仅有查询权限，请管理员放行此来源和端口"),
          permission_required: t("需要管理员或免交互 sudo 权限"),
          disabled: t("防火墙未启用，保持现状，请管理员确认网络策略"),
          unsupported: t("当前防火墙尚不支持自动配置，请管理员手动放行此来源和端口"),
          manual: t("此平台需手动配置来源 IP 与端口规则"),
          unknown: t("无法确定防火墙状态"),
          applied: t("规则已执行"),
          removed: t("本系统记录的规则已撤销"),
        }[plan.state] ?? plan.state}
      </p>
      {plan.verification === "tls_reachable" && (
        <p role="status">{t("已从 Terminal 验证 TCP/mTLS 可达。")}</p>
      )}
      {plan.verification === "pending_install" && (
        <p role="status">{t("规则已执行；请安装并连接服务管理器后确认心跳。")}</p>
      )}
      {plan.verification === "unreachable" && (
        <p role="alert">
          {t("规则已执行，但 TCP/mTLS 仍不可达。请检查服务监听、云安全组、路由器或其他网络策略。")}
        </p>
      )}
      {!plan.can_apply && plan.state === "active" && (
        <p>{t("没有可执行的规则变更，请检查本系统是否拥有该规则。")}</p>
      )}
      <details>
        <summary>{t("规则详情")}</summary>
        <code>{plan.rule}</code>
        <p>
          {t(
            "确认仅对上述来源和端口操作；五分钟后或目标状态变化后需要重新检查。只管理 Asterion 自己记录的规则。",
          )}
        </p>
      </details>
      {plan.can_apply && (
        <button
          disabled={busy || (plan.transport === "ssh" && !keyReady)}
          onClick={() => {
            if (plan.transport === "agent")
              void run("node.service_firewall", {
                id: plan.id,
                service: plan.service!,
                action: "apply",
                token: plan.token,
              });
            else {
              const key = privateKey;
              clearKey();
              void run("node.firewall.apply", { token: plan.token, private_key: key });
            }
          }}
        >
          {plan.action === "remove" ? t("确认撤销上述规则") : t("确认放行上述来源和端口")}
        </button>
      )}
    </section>
  );
}
