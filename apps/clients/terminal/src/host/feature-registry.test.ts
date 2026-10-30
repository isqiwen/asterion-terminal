import { expect, test } from "vitest";
import { readFileSync, readdirSync } from "node:fs";
import { resolve, relative } from "node:path";
import { registerTerminalFeatures } from "./feature-registry";
import type { TerminalFeature } from "../../features/contract";

const feature: TerminalFeature = {
  id: "test.panel",
  workspaces: [
    {
      id: "test.workspace",
      title: "Test",
      component: () => null,
    },
  ],
};

test("registration rejects conflicting identities and preserves registered contributions", () => {
  expect(() => registerTerminalFeatures([feature, feature])).toThrow("界面功能");
  expect(() => registerTerminalFeatures([feature, { ...feature, id: "other" }])).toThrow("工作区");
  const registered = registerTerminalFeatures([feature]);
  feature.workspaces[0].title = "Changed by caller";
  expect(registered[0].workspaces[0].title).toBe("Test");
  expect(Object.isFrozen(registered)).toBe(true);
});

test("shared code does not import an application or a concrete feature", () => {
  const root = resolve(__dirname, "../../../../..");
  function files(path: string): string[] {
    return readdirSync(path, { withFileTypes: true }).flatMap(entry => {
      const child = resolve(path, entry.name);
      return entry.isDirectory() ? files(child) : /\.(ts|tsx|hpp|cpp)$/.test(child) ? [child] : [];
    });
  }
  for (const directory of ["core", "extensions"]) {
    for (const file of files(resolve(root, directory))) {
      const source = readFileSync(file, "utf8");
      const imports = [
        ...source.matchAll(/(?:from\s*|import\s*|#include\s*)["<]([^">]+)[">]/g),
      ].map(match => match[1]);
      for (const name of imports) {
        expect(name, relative(root, file)).not.toMatch(
          /(?:apps\/|@asterion\/(?:terminal|desktop-bridge|workbench))/,
        );
        if (directory === "core") expect(name, relative(root, file)).not.toMatch(/extensions\//);
      }
    }
  }
});
