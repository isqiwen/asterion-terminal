import type { NativePluginInfo } from "../bridge/client";

export function availableDataTaskPlugins(items: readonly NativePluginInfo[]): NativePluginInfo[] {
  return items.filter(
    plugin =>
      plugin.state === "available" &&
      plugin.capabilities.some(
        capability =>
          (capability.id === "asterion.history.v2" && capability.version === 2) ||
          (capability.id === "asterion.risk.pre-trade.v1" && capability.version === 1),
      ),
  );
}

// Plugins shipped with the application are required; only installed ones are optional.
export function requiredPluginIds(items: readonly NativePluginInfo[]): Set<string> {
  return new Set(
    availableDataTaskPlugins(items)
      .filter(item => !item.managed)
      .map(item => item.id),
  );
}

// A required plugin changes version by selecting another one, never by clearing it.
export function selectPlugin(
  hashes: readonly string[],
  hash: string,
  selected: boolean,
  items: readonly NativePluginInfo[] = [],
): string[] {
  const identity = items.find(item => item.sha256 === hash)?.id;
  if (!selected)
    return identity && requiredPluginIds(items).has(identity)
      ? [...hashes]
      : hashes.filter(value => value !== hash);
  const remaining = identity
    ? hashes.filter(value => !items.some(item => item.sha256 === value && item.id === identity))
    : [...hashes];
  return remaining.includes(hash) ? remaining : [...remaining, hash];
}

export function defaultDataTaskPlugins(items: readonly NativePluginInfo[]): string[] {
  return availableDataTaskPlugins(items)
    .filter(item => !item.managed)
    .map(item => item.sha256);
}
