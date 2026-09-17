import { selectSettingsCategory, type SettingsCategory } from "./category";
import { invoke } from "@tauri-apps/api/core";
import { nativeDesktop } from "../deployment/desktop";
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
