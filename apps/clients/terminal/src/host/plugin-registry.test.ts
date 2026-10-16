import { expect, test } from "vitest";
import { readFileSync, readdirSync } from "node:fs";
import { resolve, relative } from "node:path";
import { registerTerminalPlugins, scopedContext } from "./plugin-registry";
import type { TerminalContext, TerminalPlugin } from "../../plugins/contract";

const plugin: TerminalPlugin = {
  id: "test.panel",
  commands: ["node.data_tasks.local.open"],
  workspace: {
    id: "test.workspace",
    title: "Test",
    icon: "x",
    component: () => null,
  },
};

test("registration rejects conflicting identities and preserves registered contributions", () => {
  expect(() => registerTerminalPlugins([plugin, plugin])).toThrow("UI 插件");
  expect(() => registerTerminalPlugins([plugin, { ...plugin, id: "other" }])).toThrow("工作区");
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
  await scoped.trade("node.data_tasks.local.open");
  await expect(scoped.trade("live.open", { account: "test" })).rejects.toThrow("live.open");
  await expect(
    scoped.query("data.minutes.page", {
      id: "test",
      offset: 0,
      limit: 1,
      start: "",
      end: "",
      include_macd: false,
    }),
  ).rejects.toThrow("data.minutes.page");
  expect("inspect" in scoped).toBe(false);
  expect(calls).toEqual(["node.data_tasks.local.open"]);
  expect(Object.isFrozen(registered.commands)).toBe(true);
});

test("shared code does not import an application or a concrete plugin", () => {
  const root = resolve(__dirname, "../../../../..");
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
