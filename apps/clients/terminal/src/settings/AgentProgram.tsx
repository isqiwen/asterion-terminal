import type { Snapshot, TerminalCommand } from "../bridge/client";
import { t } from "./service-state";
export function AgentProgram({
  agentProgram,
  busy,
  run,
}: {
  agentProgram: Snapshot["agent_program"];
  busy: boolean;
  run: (method: TerminalCommand, params: Record<string, string>) => Promise<boolean>;
}) {
  return (
    <details>
      <summary>{t("服务管理器程序")}</summary>
      <button disabled={busy} onClick={() => void run("node.agent.inspect", {})}>
        {t("检查程序更新")}
      </button>
      {agentProgram && (
        <>
          <p role="status">
            {
              {
                isolated: t("开发环境不管理系统安装"),
                not_installed: t("尚未安装本机服务管理器"),
                current: t("已安装程序与安装包一致"),
                update_available: t("安装包中有不同版本"),
                recovery_required: t("存在未完成的更新，请先恢复"),
              }[agentProgram.state]
            }
          </p>
          {(agentProgram.state === "update_available" ||
            agentProgram.state === "recovery_required") && (
            <>
              <p>
                {t(
                  "更新会等待服务安全停止，并在完成后恢复原运行状态；无法安全停止的业务会阻止更新。",
                )}
              </p>
              <button
                disabled={busy || !agentProgram.expected_digest}
                onClick={() =>
                  void run("node.agent.upgrade", {
                    expected_digest: agentProgram.expected_digest,
                  })
                }
              >
                {agentProgram.state === "recovery_required" ? t("继续恢复") : t("升级服务管理器")}
              </button>
            </>
          )}
          {agentProgram.bundled_digest && (
            <details>
              <summary>{t("程序摘要")}</summary>
              <p>
                {t("已安装")}: <code>{agentProgram.installed_digest || "—"}</code>
              </p>
              <p>
                {t("安装包")}: <code>{agentProgram.bundled_digest}</code>
              </p>
            </details>
          )}
        </>
      )}
    </details>
  );
}
