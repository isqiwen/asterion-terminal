import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

test("install requires explicit trust, renders declarative view and preserves unavailable selection", async ({ context, page }) => {
  await nativeContext(context);
  let installed = false;
  let enabled = false;
  let inspections = 0;
  const digest = "a".repeat(64);
  const manifest = { id: "example.calendar", title: "独立测试插件", version: "1.0.0", description: "开发测试", requires: {}, contributions: { "ui.table": {} } };
  const view = { id: "status", title: "来源状态", columns: [{ key: "value", label: "状态" }] };
  await context.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.includes("/security")) return route.fallback();
    if (path.endsWith("/access/scopes")) return route.fulfill({ json: { token: "scoped-test", expires: Date.now() / 1000 + 300 } });
    if (path.endsWith("/extensions/example.calendar/diagnostics")) return route.fulfill({ json: { digest, items: [{ id: "call-1", started: 1726800000, duration_ms: 30001, phase: "strategy.close", code: "timeout", calls: 12 }] } });
    if (path.endsWith("/extensions/inspect")) {
      inspections++;
      if (Buffer.from(route.request().postDataJSON().archive, "base64").toString() === "broken") return route.fulfill({ status: 422, json: { detail: "插件包格式无效" } });
      await new Promise((resolve) => setTimeout(resolve, 150));
      return route.fulfill({ json: { manifest, digest } });
    }
    if (path.endsWith("/extensions/install")) { expect(route.request().postDataJSON().trust_local_code).toBe(true); expect(route.request().postDataJSON().digest).toBe(digest); installed = true; enabled = true; return route.fulfill({ json: { manifest, digest, enabled } }); }
    if (path.endsWith("/extensions/example.calendar/state")) {
      const body = route.request().postDataJSON();
      expect(body.digest).toBe(digest);
      if (body.enabled) expect(body.trust_local_code).toBe(true);
      enabled = body.enabled;
      return route.fulfill({ json: { manifest, digest, enabled } });
    }
    if (path.endsWith("/extensions/example.calendar/remove")) { expect(enabled).toBe(false); installed = false; return route.fulfill({ json: { status: "removed", retained: digest } }); }
    if (path.endsWith("/extensions/views")) return route.fulfill({ json: { items: enabled ? [{ plugin_id: manifest.id, digest, view }] : [] } });
    if (path.endsWith("/extensions/example.calendar/view")) return route.fulfill({ json: { rows: [{ value: "<script>plain text</script>" }] } });
    if (path.endsWith("/extensions")) return route.fulfill({ json: { items: installed ? [{ manifest, digest, enabled }] : [] } });
    const user = { email: "plugins@example.com", first_name: "Plugin", last_name: "Test" };
    return route.fulfill({ json: path.endsWith("/login") ? { session: "session", user } : path.endsWith("/me") ? user : path.endsWith("/health") ? { status: "ready" } : [] });
  });
  const drop = async (names: string[], content = "test archive") => {
    const transfer = await page.evaluateHandle(({ names, content }) => {
      const data = new DataTransfer();
      for (const name of names) data.items.add(new File([content], name, { type: "application/zip" }));
      return data;
    }, { names, content });
    const zone = page.getByRole("region", { name: "插件包拖放区域" });
    await zone.dispatchEvent("dragover", { dataTransfer: transfer });
    await zone.dispatchEvent("drop", { dataTransfer: transfer });
    await transfer.dispose();
  };
  await page.goto("/?screen=settings");
  await page.getByLabel("邮箱", { exact: true }).fill("plugins@example.com");
  await page.getByLabel("密码", { exact: true }).fill("fixture-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page.getByRole("button", { name: "插件", exact: true }).click();
  await page.getByLabel("选择插件包").setInputFiles({ name: "fixture.zip", mimeType: "application/zip", buffer: Buffer.from("test archive") });
  await expect(page.getByText("独立测试插件", { exact: false }).first()).toBeVisible();
  await expect(page.getByRole("dialog", { name: "确认安装插件" })).toBeVisible();
  expect(installed).toBe(false);
  await page.getByRole("button", { name: "取消", exact: true }).click();
  expect(installed).toBe(false);
  await drop(["wrong.txt"]);
  await expect(page.getByRole("alert")).toContainText("请选择 ZIP");
  await drop(["one.zip", "two.zip"]);
  await expect(page.getByRole("alert")).toContainText("一次选择一个");
  await drop(["empty.zip"], "");
  await expect(page.getByRole("alert")).toContainText("插件包为空");
  expect(inspections).toBe(1);
  await drop(["broken.zip"], "broken");
  await expect(page.getByRole("alert")).toContainText("插件包格式无效");
  expect(installed).toBe(false);
  await drop(["fixture.ZIP"]);
  await drop(["duplicate.zip"]);
  await expect(page.getByRole("dialog", { name: "确认安装插件" })).toBeVisible();
  expect(inspections).toBe(3);
  expect(installed).toBe(false);
  await page.getByRole("button", { name: "信任并安装启用", exact: true }).click();
  await expect(page.getByText("已启用", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "运行诊断", exact: true }).click();
  const diagnostics = page.getByRole("region", { name: "example.calendar 运行诊断" });
  await expect(diagnostics).toContainText("strategy.close");
  await expect(diagnostics).toContainText("30001 ms");
  await expect(diagnostics).toContainText("执行超时");
  await expect(diagnostics).toContainText("减少单次工作量");
  const workspace = await context.newPage();
  await workspace.goto("/");
  await workspace.getByRole("button", { name: "扩展", exact: true }).click();
  await expect(workspace.getByRole("table", { name: "来源状态" })).toContainText("<script>plain text</script>");
  await page.getByRole("button", { name: "停用", exact: true }).click();
  await expect(page.getByText("已停用", { exact: true })).toBeVisible();
  await workspace.getByRole("button", { name: "刷新", exact: true }).click();
  await expect(workspace.getByText("插件视图不可用。", { exact: false })).toBeVisible();
  await page.getByRole("button", { name: "移除", exact: true }).click();
  await expect(page.getByText("尚未安装本地插件。", { exact: false })).toBeVisible();
});
