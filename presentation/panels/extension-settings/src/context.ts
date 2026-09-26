/** UI-owned scoped inputs; this plugin never imports the product or workspace. */
import type { RequestClient } from "@asterion/runtime-client/requests";
export type PanelContext = {

};
export type SettingsContext = {
  extensions: Readonly<{ api: RequestClient }>;
};
