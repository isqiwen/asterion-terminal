import { invoke } from "@tauri-apps/api/core";
import { nativeDesktop } from "../deployment/desktop";
import { apiBase, setAccountSession } from "../api/client";
export type AccountUser = {
  email: string;
  first_name: string;
  last_name: string;
};
export class AccountError extends Error {
  constructor(
    message: string,
    public code: string,
  ) {
    super(message);
  }
}
export type SharedSession = { revision: number; token: string | null };
export function readSharedSession(): Promise<SharedSession> {
  return invoke("desktop_account_read");
}
export function useSharedToken(value: string) {
  accountSession = value;
  setAccountSession(value);
}
export async function clearSharedSession(expected: number) {
  await invoke("desktop_account_write", { expected, token: null });
}
let accountSession = "";
export function clearAccountSession() {
  accountSession = "";
  setAccountSession("");
}
export async function accountRequest<T>(
  path: string,
  token: string,
  body?: unknown,
): Promise<T> {
  const shared =
    nativeDesktop && (path === "/login" || path === "/logout")
      ? await readSharedSession()
      : undefined;
  const response = await fetch(`${apiBase()}/api/v1/account${path}`, {
    method: body === undefined ? "GET" : "POST",
    signal: AbortSignal.timeout(45000),
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
      "X-Account-Session": accountSession,
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data = await response.json();
  if (!response.ok)
    throw new AccountError(
      typeof data.detail === "string" ? data.detail : "请检查输入后重试",
      data.code ?? "INVALID_INPUT",
    );
  if (path === "/login") {
    if (shared) {
      try {
        await invoke("desktop_account_write", {
          expected: shared.revision,
          token: data.session,
        });
      } catch (e) {
        await fetch(`${apiBase()}/api/v1/account/logout`, {
          method: "POST",
          headers: {
            Authorization: `Bearer ${token}`,
            "X-Account-Session": data.session,
          },
          signal: AbortSignal.timeout(15000),
        }).catch(() => {});
        throw e;
      }
    }
    useSharedToken(data.session);
  }
  if (path === "/logout") {
    if (shared) await clearSharedSession(shared.revision);
    clearAccountSession();
  }
  return data as T;
}
