import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { nativeDesktop } from "../bridge/desktop";
export const categoryKey = "asterion.settings.category";
export type SettingsPage = "general" | "appearance" | "connections" | "plugins" | "about";
export function settingsPage(value: string | null): SettingsPage {
  return ["general", "appearance", "connections", "plugins", "about"].includes(value ?? "") ? value as SettingsPage : "general";
}
let popup: Window | null = null;
export async function openSettings(page: SettingsPage = "general") {
  localStorage.setItem(categoryKey, JSON.stringify({page, nonce: Date.now()}));
  if (nativeDesktop) { await invoke("open_settings", {category: page}); return; }
  if (popup && !popup.closed) { popup.focus(); return; }
  popup = window.open(`/?screen=settings&category=${page}`, "asterion-settings", "popup,width=820,height=620");
  if (!popup) throw new Error("Settings window was blocked");
}
export async function closeSettings() {
  if (nativeDesktop) await getCurrentWindow().close();
  else window.close();
}
