import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { registerLanguageResources } from "../i18n";
import type { TerminalContext, TerminalPlugin } from "../../plugins/contract";
// Built-in registration only. React owns panel mount/unmount and effect cleanup.
// This is not a loader or a security boundary for untrusted extensions.
// Scopes the shared context to the commands a plugin declared.
export function scopedContext(plugin: TerminalPlugin, base: TerminalContext): TerminalContext {
  const allowed = new Set<string>(plugin.commands);
  const denied = (method: string) =>
    Promise.reject(new Error(t("插件 {p0} 未声明命令 {p1}", { p0: plugin.id, p1: method })));
  return {
    ...base,
    trade: (method, params) => (allowed.has(method) ? base.trade(method, params) : denied(method)),
    inspect: params =>
      allowed.has("futures.inspect_csv") ? base.inspect(params) : denied("futures.inspect_csv"),
  };
}
export function registerTerminalPlugins(plugins: readonly TerminalPlugin[]) {
  const ids = new Set<string>();
  const workspaces = new Set<string>();
  for (const plugin of plugins) {
    if (!plugin.id || ids.has(plugin.id))
      throw new Error(t("重复或无效的 UI 插件: {p0}", { p0: plugin.id }));
    if (plugin.apiVersion !== 1)
      throw new Error(t("不支持的 UI 插件契约: {p0}", { p0: plugin.id }));
    if (
      !Array.isArray(plugin.commands) ||
      !plugin.commands.every(command => typeof command === "string" && command.length > 0)
    )
      throw new Error(t("UI 插件命令声明无效: {p0}", { p0: plugin.id }));
    if (!plugin.workspace.id || workspaces.has(plugin.workspace.id))
      throw new Error(t("重复或无效的工作区: {p0}", { p0: plugin.workspace.id }));
    ids.add(plugin.id);
    workspaces.add(plugin.workspace.id);
  }
  for (const plugin of plugins)
    if (plugin.languageResources) registerLanguageResources(plugin.id, plugin.languageResources);
  return Object.freeze(
    plugins.map(plugin =>
      Object.freeze({
        ...plugin,
        commands: Object.freeze([...plugin.commands]),
        workspace: Object.freeze(
          Object.defineProperties({}, Object.getOwnPropertyDescriptors(plugin.workspace)),
        ) as TerminalPlugin["workspace"],
      }),
    ),
  );
}
