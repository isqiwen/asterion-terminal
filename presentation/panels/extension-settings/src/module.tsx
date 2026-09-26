import type { UiModule } from "@asterion/workbench/extensions/modules";
import type { PanelContext } from "./context";
import type { SettingsContext } from "./context";
import { Extensions } from "./Extensions";

export const uiModule = { 
  id: "asterion.ui.extension-settings",
  settings: [{ id: "terminal.extensions", scope: "extensions", title: "插件", order: 3.5,
    render: (context) => <Extensions api={context.api} /> }],
} satisfies UiModule<PanelContext, SettingsContext>;
