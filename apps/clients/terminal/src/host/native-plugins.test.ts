import { expect, test } from "vitest";
import type { NativePluginInfo } from "../bridge/client";
import {
  availableDataTaskPlugins,
  defaultDataTaskPlugins,
  requiredPluginIds,
  selectPlugin,
} from "./native-plugins";

test("table-only bundled plugins are enabled and required while installed ones remain optional", () => {
  const bundled: NativePluginInfo = {
    id: "asterion.data.shfe",
    file: "shfe.so",
    version: "1.0.0",
    sha256: "a".repeat(64),
    capabilities: [{ id: "asterion.history.tables.v1", version: 1, kind: "data" }],
    state: "available",
    error: "",
  };
  const installed: NativePluginInfo = {
    ...bundled,
    id: "installed.tables",
    file: "installed.so",
    sha256: "b".repeat(64),
    managed: true,
  };
  const items = [bundled, installed];
  expect(availableDataTaskPlugins(items)).toEqual(items);
  const defaults = defaultDataTaskPlugins(items);
  expect(defaults).toEqual([bundled.sha256]);
  expect(requiredPluginIds(items)).toEqual(new Set([bundled.id]));
  expect(selectPlugin(defaults, bundled.sha256, false, items)).toEqual(defaults);
  const selected = selectPlugin(defaults, installed.sha256, true, items);
  expect(selected).toEqual([bundled.sha256, installed.sha256]);
  expect(selectPlugin(selected, installed.sha256, false, items)).toEqual(defaults);
});
