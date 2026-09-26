import { afterEach, expect, test, vi } from "vitest";
import { configureApi, createRequestScope as rawScope, setAccountSession } from "./client";
const grants = [
  { path: "/fixture", descendants: true, methods: ["GET", "POST"] as const },
];
afterEach(() => {
  vi.unstubAllGlobals();
  setAccountSession("");
  configureApi("http://127.0.0.1:8000");
});
test("credential-free clients attach the captured identity and preserve exact text", async () => {
  setAccountSession("account-one");
  const fetcher = vi
    .fn()
    .mockResolvedValue(new Response('{"value":1.000000000000000001}'));
  stubFetch(fetcher);
  const { client } = createRequestScope("local-secret", true, grants);
  expect(Object.keys(client).sort()).toEqual(["key", "request", "text"]);
  expect(await client.text("/fixture/report")).toBe(
    '{"value":1.000000000000000001}',
  );
  expect(fetcher).toHaveBeenCalledWith(
    "http://127.0.0.1:8000/api/v1/fixture/report",
    expect.objectContaining({
      method: "GET",
      redirect: "error",
      headers: expect.objectContaining({
        Authorization: "Bearer scoped-fixture",
        "X-Account-Session": "account-one",
      }),
    }),
  );
});
test.each([
  "/account/login",
  "/fixture-other",
  "/fixture/../account",
  "/fixture/%2e%2e/account",
  "/fixture/%252e%252e/account",
  "/fixture\\account",
  "//elsewhere/fixture",
  "https://elsewhere/fixture",
  "/fixture#fragment",
])("rejects out-of-scope or ambiguous path %s before fetch", async (path) => {
  const fetcher = vi.fn();
  stubFetch(fetcher);
  const { client } = createRequestScope("secret", true, grants);
  await expect(client.request(path)).rejects.toThrow("授权范围");
  expect(fetcher).not.toHaveBeenCalled();
});
test("method grants are copied and cannot be expanded by mutating the input", async () => {
  const policy = [
    {
      path: "/fixture",
      descendants: true,
      methods: ["GET"] as ("GET" | "POST")[],
    },
  ];
  const fetcher = vi.fn();
  stubFetch(fetcher);
  const { client } = createRequestScope("secret", true, policy);
  policy[0].methods.push("POST");
  await expect(client.request("/fixture", {})).rejects.toThrow("授权范围");
  expect(fetcher).not.toHaveBeenCalled();
});
test("disconnect and revoked clients reject new requests", async () => {
  const fetcher = vi.fn();
  stubFetch(fetcher);
  const scope = createRequestScope("secret", true, grants);
  scope.revoke();
  await expect(scope.client.request("/fixture")).rejects.toThrow("连接或账户");
  await expect(
    createRequestScope("secret", false, grants).client.request("/fixture"),
  ).rejects.toThrow("连接或账户");
  expect(fetcher).not.toHaveBeenCalled();
});
test("account changes reject both retained clients and responses already in flight", async () => {
  setAccountSession("first");
  let finish!: (response: Response) => void;
  stubFetch(
    vi.fn(
      () =>
        new Promise<Response>((resolve) => {
          finish = resolve;
        }),
    ),
  );
  const { client } = createRequestScope("secret", true, grants);
  const pending = client.request("/fixture");
  await vi.waitFor(() => expect(finish).toBeTypeOf("function"));
  setAccountSession("second");
  finish(new Response('{"owner":"first"}'));
  await expect(pending).rejects.toThrow("连接或账户");
  await expect(client.request("/fixture")).rejects.toThrow("连接或账户");
});
test("changing runtime address invalidates existing clients", async () => {
  const fetcher = vi.fn();
  stubFetch(fetcher);
  const { client } = createRequestScope("secret", true, grants);
  configureApi("http://127.0.0.1:9001");
  await expect(client.request("/fixture")).rejects.toThrow("连接或账户");
  expect(fetcher).not.toHaveBeenCalled();
});
test("revocation aborts in-flight work even after lifecycle reactivation", async () => {
  let finish!: (response: Response) => void;
  let signal!: AbortSignal;
  stubFetch(
    vi.fn((_, init) => {
      signal = init.signal;
      return new Promise<Response>((resolve) => {
        finish = resolve;
      });
    }),
  );
  const scope = createRequestScope("secret", true, grants);
  const pending = scope.client.request("/fixture");
  await vi.waitFor(() => expect(finish).toBeTypeOf("function"));
  scope.revoke();
  scope.activate();
  expect(signal.aborted).toBe(true);
  finish(new Response("{}"));
  await expect(pending).rejects.toThrow("已取消");
});

test("exact parameterized grants do not authorize sibling or descendant operations", async () => {
  const fetcher = vi.fn().mockResolvedValue(new Response("{}"));
  stubFetch(fetcher);
  const { client } = createRequestScope("secret", true, [
    { path: "/fixture/:id/coverage", descendants: false, methods: ["POST"] },
  ]);
  await expect(
    client.request("/fixture/version-1/coverage", {}),
  ).resolves.toEqual({});
  await expect(
    client.request("/fixture/version-1/archive", {}),
  ).rejects.toThrow("授权范围");
  await expect(
    client.request("/fixture/version-1/coverage/remove", {}),
  ).rejects.toThrow("授权范围");
  expect(fetcher).toHaveBeenCalledTimes(1);
});

function createRequestScope(token: string, connected: boolean, grants: Parameters<typeof rawScope>[2]) {
  return rawScope(token, connected, grants, "fixture");
}
function stubFetch(handler: (...args: any[]) => any) {
  vi.stubGlobal("fetch", (url: string, init: RequestInit) => url.endsWith("/access/scopes")
    ? Promise.resolve(new Response(JSON.stringify({ token: "scoped-fixture", expires: Date.now() / 1000 + 300 })))
    : handler(url, init));
}
