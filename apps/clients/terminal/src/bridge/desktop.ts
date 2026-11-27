export interface MarketWatchBatch {
  frames: Uint8Array[];
  online: boolean;
  error: string;
}
export interface DesktopBridge {
  readonly platform: string;
  request(body: string): Promise<string>;
  marketWatch?(restart: boolean): Promise<MarketWatchBatch | null>;
  lab(payload: Uint8Array): Promise<Uint8Array>;
  openSettings(category: string): Promise<void>;
  openLogs(): Promise<void>;
  setTitle(title: string): Promise<void>;
  close(): Promise<void>;
}
declare global {
  interface Window {
    asterionDesktop?: DesktopBridge;
  }
}
export const nativeDesktop = typeof window.asterionDesktop !== "undefined";
export function desktop(): DesktopBridge {
  if (!window.asterionDesktop) throw new Error("Desktop bridge is unavailable");
  return window.asterionDesktop;
}
