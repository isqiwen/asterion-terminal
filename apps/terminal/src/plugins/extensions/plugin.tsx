import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
import { Extensions } from "./Extensions";

export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.extensions",
  requires: [],
  settings: [{ id: "terminal.extensions", scope: "extensions", title: "插件", order: 3.5,
    render: (context) => <Extensions api={context.api} /> }],
};
