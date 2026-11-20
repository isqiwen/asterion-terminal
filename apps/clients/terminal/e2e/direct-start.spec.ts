import { test, expect } from "./test";

test("workbench starts without platform authentication while the local bridge keeps origin checks", async ({
  page,
  request,
}) => {
  const authRequests: string[] = [];
  page.on("request", event => {
    if (event.url().includes("/api/v1/auth")) authRequests.push(event.url());
  });
  await page.route("**/api/v1/auth/**", route => route.abort());
  await page.goto("/");
  await expect(page.locator(".activity-rail")).toBeVisible();
  await expect(page.getByRole("heading", { name: "登录", exact: true })).toHaveCount(0);
  await expect(page.getByRole("button", { name: "退出登录", exact: true })).toHaveCount(0);
  const response = await request.post("/__asterion/api", {
    data: { version: 1, method: "runtime.snapshot", params: {} },
  });
  expect(response.status()).toBe(200);
  expect((await response.json()).result.protocol).toBe(1);
  const forbidden = await request.post("/__asterion/api", {
    headers: { Origin: "https://untrusted.example" },
    data: { version: 1, method: "runtime.snapshot", params: {} },
  });
  expect(forbidden.status()).toBe(403);
  await page.reload();
  await expect(page.locator(".activity-rail")).toBeVisible();
  expect(authRequests).toEqual([]);
});
