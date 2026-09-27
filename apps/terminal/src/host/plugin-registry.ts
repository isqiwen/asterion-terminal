import { translate, type MessageValues } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { registerLanguageResources } from "../i18n";
import type { TerminalPlugin } from "../../plugins/contract";
// Built-in registration only. React owns panel mount/unmount and effect cleanup.
// This is not a loader or a security boundary for untrusted extensions.
export function registerTerminalPlugins(plugins: readonly TerminalPlugin[]) {
    const ids = new Set<string>();
    const workspaces = new Set<string>();
    for (const plugin of plugins) {
        if (!plugin.id || ids.has(plugin.id))
            throw new Error(t("重复或无效的 UI 插件: {p0}", { p0: plugin.id }));
        if (plugin.apiVersion !== 1)
            throw new Error(t("不支持的 UI 插件契约: {p0}", { p0: plugin.id }));
        if (!plugin.workspace.id || workspaces.has(plugin.workspace.id))
            throw new Error(t("重复或无效的工作区: {p0}", { p0: plugin.workspace.id }));
        ids.add(plugin.id);
        workspaces.add(plugin.workspace.id);
    }
    for (const plugin of plugins)
        if (plugin.languageResources)
            registerLanguageResources(plugin.id, plugin.languageResources);
    return Object.freeze(plugins.map(plugin => Object.freeze({ ...plugin, workspace: Object.freeze(Object.defineProperties({}, Object.getOwnPropertyDescriptors(plugin.workspace))) as TerminalPlugin["workspace"] })));
}
