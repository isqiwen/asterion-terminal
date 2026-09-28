import { translate, type TerminalContext } from "../contract";
const t = (key: string, values?: Record<string, string | number>) =>
  translate("asterion.terminal.overview", key, values);
export function ServiceSummary({ context }: { context: TerminalContext }) {
  const nodes = context.snapshot?.nodes ?? [];
  const services = nodes.flatMap(n => n.health?.services ?? []);
  const issues =
    nodes.filter(n => n.state !== "online").length +
    nodes
      .filter(n => n.state === "online")
      .flatMap(n => n.health?.services ?? [])
      .filter(
        s => s.desired_running && (s.state !== "running" || s.health === "degraded" || !!s.error),
      ).length;
  const status =
    !context.snapshot || context.error || nodes.some(n => n.state === "online" && !n.health)
      ? t("状态待确认")
      : issues
        ? t("{count} 项需关注", { count: issues })
        : services.length
          ? t("{count} 个服务 · {running} 个运行中", {
              count: services.length,
              running: services.filter(s => s.state === "running").length,
            })
          : t("尚未启用后台服务");
  return (
    <button
      className={`overview-service-status ${issues ? "bad" : ""}`}
      onClick={() => context.openSettings("connections")}
      aria-label={t("查看服务状态")}
    >
      <span aria-hidden="true">◉</span>
      {status}
      <span aria-hidden="true">↗</span>
    </button>
  );
}
