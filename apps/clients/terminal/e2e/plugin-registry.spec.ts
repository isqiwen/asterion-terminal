import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";
import { readFileSync, readdirSync } from "node:fs";
import { resolve, relative } from "node:path";
import { registerTerminalPlugins, scopedContext } from "../src/host/plugin-registry";
import type { TerminalContext, TerminalPlugin } from "../plugins/contract";

const plugin: TerminalPlugin = {
  id: "test.panel",
  apiVersion: 1,
  commands: ["research.local"],
  workspace: {
    id: "test.workspace",
    title: "Test",
    icon: "x",
    component: () => null,
  },
};

test("registration rejects conflicting identities and unsupported contracts", () => {
  expect(() => registerTerminalPlugins([plugin, plugin])).toThrow("UI 插件");
  expect(() => registerTerminalPlugins([plugin, { ...plugin, id: "other" }])).toThrow("工作区");
  expect(() =>
    registerTerminalPlugins([{ ...plugin, apiVersion: 2 } as unknown as typeof plugin]),
  ).toThrow("契约");
  const registered = registerTerminalPlugins([plugin]);
  plugin.workspace.title = "Changed by caller";
  expect(registered[0].workspace.title).toBe("Test");
  expect(Object.isFrozen(registered)).toBe(true);
});

test("plugins can only invoke the commands they declare", async () => {
  expect(() =>
    registerTerminalPlugins([{ ...plugin, commands: undefined } as unknown as TerminalPlugin]),
  ).toThrow("命令声明");
  const calls: string[] = [];
  const base = {
    trade: async (method: string) => void calls.push(method),
  } as unknown as TerminalContext;
  const [registered] = registerTerminalPlugins([plugin]);
  const scoped = scopedContext(registered, base);
  await scoped.trade("research.local");
  await expect(scoped.trade("live.act")).rejects.toThrow("live.act");
  await expect(scoped.query("research.minutes.page", {})).rejects.toThrow("research.minutes.page");
  expect("inspect" in scoped).toBe(false);
  expect(calls).toEqual(["research.local"]);
  expect(Object.isFrozen(registered.commands)).toBe(true);
});

test("shared code does not import an application or a concrete plugin", () => {
  const root = resolve(__dirname, "../../../..");
  function files(path: string): string[] {
    return readdirSync(path, { withFileTypes: true }).flatMap(entry => {
      const child = resolve(path, entry.name);
      return entry.isDirectory() ? files(child) : /\.(ts|tsx|hpp|cpp)$/.test(child) ? [child] : [];
    });
  }
  for (const directory of ["core", "plugins"]) {
    for (const file of files(resolve(root, directory))) {
      const source = readFileSync(file, "utf8");
      const imports = [
        ...source.matchAll(/(?:from\s*|import\s*|#include\s*)["<]([^">]+)[">]/g),
      ].map(match => match[1]);
      for (const name of imports) {
        expect(name, relative(root, file)).not.toMatch(
          /(?:apps\/|@asterion\/(?:terminal|desktop-bridge|workbench))/,
        );
        if (directory === "core") expect(name, relative(root, file)).not.toContain("plugins/");
      }
    }
  }
});

test("Terminal registry drives settings and workspace navigation", async ({ page: workbench }) => {
  await workbench.goto("/");
  const settings = await openSettingsWindow(workbench);
  await settings.getByRole("button", { name: "插件", exact: true }).click();
  await expect(settings.getByText("已注册 · 按需加载", { exact: true })).toHaveCount(6);
  const page = await closeSettingsWindow(settings);
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByRole("heading", { name: "均线回测", exact: true })).toBeVisible();
  await page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }).click();
  await expect(page.getByRole("heading", { name: "CTP 交易", exact: true })).toBeVisible();
});
