import type { NativePluginInfo } from "../bridge/client";

export function availableResearchPlugins(items: readonly NativePluginInfo[]): NativePluginInfo[] {
  return items.filter(
    plugin =>
      plugin.state === "available" &&
      plugin.capabilities.some(
        capability =>
          (capability.id === "asterion.history.v1" ||
            capability.id === "asterion.risk.pre-trade.v1") &&
          capability.version === 1,
      ),
  );
}

export function selectPlugin(
  hashes: readonly string[],
  hash: string,
  selected: boolean,
  items: readonly NativePluginInfo[] = [],
): string[] {
  if (!selected) return hashes.filter(value => value !== hash);
  const identity = items.find(item => item.sha256 === hash)?.id;
  const remaining = identity
    ? hashes.filter(value => !items.some(item => item.sha256 === value && item.id === identity))
    : [...hashes];
  return remaining.includes(hash) ? remaining : [...remaining, hash];
}

export function defaultResearchPlugins(items: readonly NativePluginInfo[]): string[] {
  const candidates = availableResearchPlugins(items);
  return candidates
    .filter(item => candidates.filter(other => other.id === item.id).length === 1)
    .map(item => item.sha256);
}
