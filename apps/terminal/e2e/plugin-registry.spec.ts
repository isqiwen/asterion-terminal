import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { readFileSync, readdirSync } from "node:fs";
import { resolve, relative } from "node:path";
import { registerTerminalPlugins } from "../src/host/plugin-registry";
import type { TerminalPlugin } from "../plugins/contract";

const plugin: TerminalPlugin = {
  id: "test.panel",
  apiVersion: 1,
  workspace: {
    id: "test.workspace",
    title: "Test",
    icon: "x",
    section: "Test",
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

test("shared code does not import an application or a concrete plugin", () => {
  const root = resolve(__dirname, "../../..");
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
          /(?:apps\/|@asterion\/(?:terminal|desktop-bridge|workbench|overview))/,
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
  await expect(settings.getByText("已注册 · 按需加载", { exact: true })).toHaveCount(5);
  const page = await closeSettingsWindow(settings);
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByRole("heading", { name: "均线回测", exact: true })).toBeVisible();
  await page.getByRole("button", { name: "交易", exact: true }).click();
  await expect(page.getByRole("heading", { name: "期货模拟交易", exact: true })).toBeVisible();
});
