import type { SettingsContext as DataSettingsContext } from "@asterion/ui-data-panel/context";
import type { SettingsContext as AppearanceSettingsContext } from "@asterion/ui-appearance-settings/context";
import type { SettingsContext as ConnectionSettingsContext } from "@asterion/ui-connection-settings/context";
import type { SettingsContext as ExtensionSettingsContext } from "@asterion/ui-extension-settings/context";
import type { SettingsContext as AccountSettingsContext } from "@asterion/ui-account-settings/context";
import type { SettingsContext as MaintenanceSettingsContext } from "@asterion/ui-maintenance-panel/context";
export type SettingsContext = DataSettingsContext & AppearanceSettingsContext & ConnectionSettingsContext & ExtensionSettingsContext & AccountSettingsContext & MaintenanceSettingsContext;
