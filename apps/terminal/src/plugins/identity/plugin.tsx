import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
import { SecurityPreferences } from "./TerminalSecurity";
export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.identity",
  requires: [],
  settings: [
    {
      id: "identity.security",
      scope: "empty",
      title: "安全",
      order: 1,
      render: () => <SecurityPreferences />,
    },
  ],
};
