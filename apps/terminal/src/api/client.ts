let accountSession = "";
export function setAccountSession(value: string) {
  if (accountSession === value) return;
  accountSession = value;
  sessionChanged();
}
let apiUrl = "http://127.0.0.1:8000";
export function apiBase() {
  return apiUrl;
}
export function configureApi(url: string) {
  const parsed = new URL(url);
  if (parsed.protocol !== "http:" || parsed.hostname !== "127.0.0.1")
    throw new Error("Invalid local runtime address");
  if (apiUrl === url) return;
  apiUrl = url;
  sessionChanged();
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

export type { RequestClient, RequestGrant } from "./requests";
import type { RequestClient, RequestGrant } from "./requests";
let sessionRevision = 0;
const sessionListeners = new Set<() => void>();
export const subscribeSession = (listener: () => void) => {
  sessionListeners.add(listener);
  return () => {
    sessionListeners.delete(listener);
  };
};
export const currentSessionRevision = () => sessionRevision;
function sessionChanged() {
  sessionRevision++;
  sessionListeners.forEach((listener) => listener());
}
let nextClient = 0;
export function createRequestScope(
  token: string,
  connected: boolean,
  grants: readonly RequestGrant[],
  scopeId: string,
) {
  const session = accountSession;
  const revision = sessionRevision;
  const base = apiUrl;
  const allowed = grants.map((grant) => ({
    path: grant.path,
    descendants: grant.descendants,
    methods: [...grant.methods],
  }));
  let credential: Promise<{ token: string; expires: number }> | undefined;
  let credentialExpires = 0;
  let live = true;
  let lifetime = new AbortController();
  const check = () => {
    if (
      !live ||
      !connected ||
      !token ||
      revision !== sessionRevision ||
      base !== apiUrl
    )
      throw new Error("连接或账户已变化，请重新载入后重试");
  };
  async function send(
    path: string,
    body: string | undefined,
    timeout: number,
    text: boolean,
  ) {
    check();
    const method = body === undefined ? "GET" : "POST";
    // Reject normalization and encoded path separators before attaching credentials.
    const pathname = path.split("?")[0];
    if (
      !/^\/[a-zA-Z0-9_./-]+$/.test(pathname) ||
      pathname.split("/").some((part) => part === "." || part === "..") ||
      path.includes("#") ||
      pathname.includes("//") ||
      !allowed.some(
        (grant) =>
          matchesPath(grant.path, pathname, grant.descendants) &&
          grant.methods.includes(method),
      )
    )
      throw new Error("请求超出当前功能授权范围");
    const signal = AbortSignal.any([
      lifetime.signal,
      AbortSignal.timeout(timeout),
    ]);
    if (!credential || credentialExpires < Date.now() / 1000 + 10) {
      credentialExpires = Infinity;
      credential = fetch(`${base}/api/v1/access/scopes`, {
        signal, method: "POST", redirect: "error",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json", "X-Account-Session": session },
        body: JSON.stringify({ scope: scopeId }),
      }).then(async (response) => {
        check();
        if (signal.aborted) throw new Error("请求已取消");
        if (!response.ok) throw new Error("无法取得当前功能的服务端授权");
        const value = await response.json();
        if (typeof value.token !== "string" || typeof value.expires !== "number") throw new Error("服务端授权响应无效");
        credentialExpires = value.expires;
        return value;
      }).catch((error) => { credential = undefined; credentialExpires = 0; throw error; });
    }
    const scoped = await credential;
    check();
    if (signal.aborted) throw new Error("请求已取消");
    const response = await fetch(`${base}/api/v1${path}`, {
      signal,
      method,
      headers: {
        Authorization: `Bearer ${scoped.token}`,
        "Content-Type": "application/json",
        "X-Account-Session": session,
      },
      body,
      redirect: "error",
    });
    check();
    if (signal.aborted) throw new Error("请求已取消");
    if (!response.ok) {
      const value = await response.json().catch(() => null);
      check();
      throw new Error(
        typeof value?.detail === "string"
          ? value.detail
          : `请求失败 (${response.status})`,
      );
    }
    const value = await (text ? response.text() : response.json());
    check();
    if (signal.aborted) throw new Error("请求已取消");
    return value;
  }
  const client: RequestClient = Object.freeze({
    key: `request-scope-${++nextClient}`,
    request: <T>(path: string, body?: unknown, timeout = 15000): Promise<T> =>
      send(
        path,
        body === undefined ? undefined : JSON.stringify(body),
        timeout,
        false,
      ),
    text: (path: string, body?: string): Promise<string> =>
      send(path, body, 60000, true),
  });
  return {
    client,
    activate() {
      live = true;
      if (lifetime.signal.aborted) lifetime = new AbortController();
    },
    revoke() {
      live = false;
      lifetime.abort();
    },
  };
}

function matchesPath(pattern: string, pathname: string, descendants: boolean) {
  const expected = pattern.split("/");
  const actual = pathname.split("/");
  if (
    descendants
      ? actual.length < expected.length
      : actual.length !== expected.length
  )
    return false;
  return expected.every((part, index) =>
    part === ":id" ? !!actual[index] : part === actual[index],
  );
}
