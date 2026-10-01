import { nativeDesktop, desktop } from "../bridge/desktop";
export const categoryKey = "asterion.settings.category";
export const selectedPageKey = "asterion.settings.selected-page";
export type SettingsPage = "preferences" | "connections" | "sources" | "plugins" | "about";
export function settingsPage(value: string | null): SettingsPage {
  return ["preferences", "connections", "sources", "plugins", "about"].includes(value ?? "")
    ? (value as SettingsPage)
    : "preferences";
}
let popup: Window | null = null;
export async function openSettings(page?: SettingsPage) {
  if (page) localStorage.setItem(categoryKey, JSON.stringify({ page, nonce: Date.now() }));
  const selected = page ?? settingsPage(localStorage.getItem(selectedPageKey));
  if (nativeDesktop) {
    await desktop().openSettings(selected);
    return;
  }
  if (popup && !popup.closed) {
    popup.focus();
    return;
  }
  popup = window.open(
    `/?screen=settings&category=${selected}`,
    "asterion-settings",
    `popup,width=760,height=540,left=${Math.round(window.screenX + (window.outerWidth - 760) / 2)},top=${Math.round(window.screenY + (window.outerHeight - 540) / 2)}`,
  );
  if (!popup) throw new Error("Settings window was blocked");
}
export async function closeSettings() {
  if (nativeDesktop) await desktop().close();
  else {
    window.opener?.focus();
    window.close();
  }
}
