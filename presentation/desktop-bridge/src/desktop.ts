import { invoke } from "./native";
import { isTauri } from "@tauri-apps/api/core";
export const nativeDesktop = isTauri();
export type DesktopSession = {
  api_url: string;
  token: string;
  data_directory: string;
};
let pending: Promise<DesktopSession> | undefined;
export function startDesktop(): Promise<DesktopSession> {
  pending ??= invoke<DesktopSession>("desktop_session").finally(() => {
    pending = undefined;
  });
  return pending;
}
export async function stopDesktop() {
  await invoke("desktop_stop");
  pending = undefined;
}
