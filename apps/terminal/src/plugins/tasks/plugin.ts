import { widgets } from "./widgets";
import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  extensions: widgets,
  id: "asterion.tasks",
  requires: ["asterion.overview", "asterion.identity"],
};
