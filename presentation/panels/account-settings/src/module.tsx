import type { UiModule } from "@asterion/workbench/extensions/modules";
import type { PanelContext } from "./context";
import type { SettingsContext } from "./context";
import { SecurityPreferences } from "./TerminalSecurity";
export const uiModule = { 
  id: "asterion.ui.account-settings",
  settings: [
    {
      id: "identity.security",
      scope: "empty",
      title: "安全",
      order: 1,
      render: () => <SecurityPreferences />,
    },
  ],
} satisfies UiModule<PanelContext, SettingsContext>;
