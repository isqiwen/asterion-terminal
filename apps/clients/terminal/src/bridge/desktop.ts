export type DialogOptions = {
  title?: string;
  defaultPath?: string;
  directory?: boolean;
  multiple?: boolean;
  filters?: { name: string; extensions: string[] }[];
};
export interface DesktopBridge {
  readonly platform: string;
  readonly environment: "development" | "production";
  onDevelopmentStopping(listener: (stopping: boolean) => void): () => void;
  request(body: string): Promise<string>;
  openSettings(category: string): Promise<void>;
  open(options: DialogOptions): Promise<string | string[] | null>;
  save(options: DialogOptions): Promise<string | null>;
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
export const open = (options: DialogOptions) => desktop().open(options);
export const save = (options: DialogOptions) => desktop().save(options);

export const developmentEnvironment = nativeDesktop
  ? desktop().environment === "development"
  : import.meta.env.DEV;
