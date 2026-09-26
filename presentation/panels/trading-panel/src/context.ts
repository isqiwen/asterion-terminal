/** UI-owned scoped inputs; this plugin never imports the product or workspace. */
import type { RequestClient } from "@asterion/runtime-client/requests";
type Connection = Readonly<{ api: RequestClient; connected: boolean }>;
export type PanelContext = {
  trading: Connection;
};
export type SettingsContext = {

};
