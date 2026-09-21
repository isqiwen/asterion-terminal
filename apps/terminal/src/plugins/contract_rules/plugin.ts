import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";

/** Public rules editor is consumed by the declared research dependency. */
export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  requires: ["asterion.data", "asterion.trading_time"],
  apiVersion: 1,
  id: "asterion.contract_rules",
};
