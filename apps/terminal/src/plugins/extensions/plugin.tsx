import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
import { Views } from "./Views";
import { Extensions } from "./Extensions";

export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.extensions",
  requires: [],
  workspaces: [{ id: "workspace.extensions", title: "扩展", icon: "◇", sections: [{ title: "插件视图", panel: "extensions.views" }] }],
  panels: [{ id: "extensions.views", scope: "extensions", render: (context) => <Views api={context.api} /> }],
  settings: [{ id: "terminal.extensions", scope: "extensions", title: "插件", order: 3.5,
    render: (context) => <Extensions api={context.api} /> }],
};
