import { existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { expect, test } from "vitest";
import { distribution } from "./distribution";

type Manifest = {
  name: string;
  asterion: { layer: number; kind: string; id?: string };
  exports?: Record<string, string>;
  dependencies?: Record<string, string>;
  devDependencies?: Record<string, string>;
};
const root = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
const paths = [
  ...["presentation", "presentation/panels"].flatMap(base => readdirSync(resolve(root, base), { withFileTypes: true }).filter(entry => entry.isDirectory() && `${base}/${entry.name}` !== "presentation/panels").map(entry => `${base}/${entry.name}`)),
  "bindings/typescript", "products/terminal",
];
const packages = paths.map(path => ({
  directory: resolve(root, path),
  manifest: JSON.parse(readFileSync(resolve(root, path, "package.json"), "utf8")) as Manifest,
}));
const byName = new Map(packages.map(item => [item.manifest.name, item]));
function sources(directory: string): string[] {
  return readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    const path = resolve(directory, entry.name);
    return entry.isDirectory() ? sources(path) : /\.(ts|tsx)$/.test(path) ? [path] : [];
  });
}

test("source packages declare one layer, exported imports and an acyclic dependency graph", () => {
  const visiting = new Set<string>();
  const visited = new Set<string>();
  function visit(name: string) {
    expect(visiting.has(name), `Package dependency cycle at ${name}`).toBe(false);
    if (visited.has(name)) return;
    visiting.add(name);
    const { manifest } = byName.get(name)!;
    for (const dependency of Object.keys(manifest.dependencies ?? {})) {
      if (byName.has(dependency)) visit(dependency);
    }
    visiting.delete(name);
    visited.add(name);
  }
  for (const { directory, manifest } of packages) {
    expect(manifest.asterion.layer).toBe(manifest.name === "@asterion/product-terminal" ? 5 : 4);
    visit(manifest.name);
    for (const target of Object.values(manifest.exports ?? {})) {
      expect(existsSync(resolve(directory, target)), `${manifest.name} exports missing file ${target}`).toBe(true);
    }
    for (const file of sources(resolve(directory, "src"))) {
      const source = readFileSync(file, "utf8");
      const imports = [...source.matchAll(/(?:from\s+|import\s*(?:\(\s*)?)["']([^"']+)["']/g)].map(match => match[1]);
      for (const specifier of imports) {
        if (specifier.startsWith(".")) {
          const resolved = resolve(dirname(file), specifier);
          const sharedTestFixture = /\.test\.tsx?$/.test(file) && specifier.endsWith(".json") && !relative(resolve(root, "contracts"), resolved).startsWith("..");
          if (!sharedTestFixture) expect(relative(directory, resolved).startsWith(".."), `${file} bypasses a package boundary`).toBe(false);
          continue;
        }
        if (!specifier.startsWith("@asterion/")) continue;
        const dependency = specifier.split("/").slice(0, 2).join("/");
        const target = byName.get(dependency);
        expect(target, `Unknown package ${specifier} in ${file}`).toBeDefined();
        expect({ ...manifest.dependencies, ...manifest.devDependencies }[dependency], `Undeclared dependency ${specifier} in ${file}`).toBe("workspace:*");
        const subpath = `.${specifier.slice(dependency.length)}`;
        expect(target!.manifest.exports?.[subpath], `Private import ${specifier} in ${file}`).toBeDefined();
        expect(target!.manifest.asterion.layer).toBeLessThanOrEqual(manifest.asterion.layer);
        if (manifest.asterion.kind === "presentation") expect(target!.manifest.asterion.kind).not.toBe("panel");
      }
    }
  }
});

test("the product composes every panel package exactly once, with no run-time dependency declarations", () => {
  const selected = new Map(distribution.modules.map(module => [module.id, module]));
  expect(selected.size).toBe(distribution.modules.length);
  const panels = packages.filter(item => item.manifest.asterion.kind === "panel");
  for (const { manifest } of panels) {
    const module = selected.get(manifest.asterion.id!);
    expect(module, `${manifest.name} is never composed`).toBeDefined();
    for (const key of ["requires", "apiVersion", "layer"]) expect(key in module!, `${manifest.name} declares ${key}`).toBe(false);
  }
  expect(panels.length).toBe(distribution.modules.length);
});
