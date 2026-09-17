import type { components } from "./schema";
export type Provider = components["schemas"]["ProviderStatus"];
export type Job = components["schemas"]["Job"];
export type Snapshot = components["schemas"]["Snapshot"];
export type Bar = components["schemas"]["Bar"];
let accountSession = "";
export function setAccountSession(value: string) {
  accountSession = value;
}
let apiUrl = "http://127.0.0.1:8000";
export function apiBase() {
  return apiUrl;
}
export function configureApi(url: string) {
  const parsed = new URL(url);
  if (parsed.protocol !== "http:" || parsed.hostname !== "127.0.0.1")
    throw new Error("Invalid local runtime address");
  apiUrl = url;
}
export async function request<T>(
  path: string,
  token: string,
  body?: unknown,
  timeout = 15000,
): Promise<T> {
  const response = await fetch(`${apiUrl}/api/v1${path}`, {
    signal: AbortSignal.timeout(timeout),
    method: body === undefined ? "GET" : "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
      "X-Account-Session": accountSession,
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (!response.ok) {
    const error = await response.json().catch(() => null);
    throw new Error(
      typeof error?.detail === "string"
        ? error.detail
        : `请求失败 (${response.status})`,
    );
  }
  return response.json();
}
