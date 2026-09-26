/** Every application-owned native command uses the shared communication context. */
import { invoke as nativeInvoke } from "@tauri-apps/api/core";
import { createContext } from "@asterion/runtime-client/communication";

export function invoke<T>(
  command: string,
  args: Record<string, unknown> = {},
): Promise<T> {
  return nativeInvoke<T>(command, {
    ...args,
    communication: createContext(600_000),
  });
}
