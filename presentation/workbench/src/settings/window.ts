import { selectSettingsCategory, type SettingsCategory } from "./category";
import { invoke } from "@asterion/desktop-bridge/native";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
export async function openSettings(category?: SettingsCategory) {
  if (category) selectSettingsCategory(category);
  if (nativeDesktop) await invoke("open_settings");
  else
    window.open(
      "/?screen=settings",
      "asterion-settings",
      "popup,width=820,height=620",
    );
}
