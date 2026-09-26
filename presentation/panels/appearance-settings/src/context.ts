/** UI-owned scoped inputs; this plugin never imports the product or workspace. */
import type { Preferences } from "@asterion/ui-kit/preferences";
export type PanelContext = {

};
export type SettingsContext = {
  appearance: Readonly<{ preferences: Preferences }>;
};
