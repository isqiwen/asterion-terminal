import type { Snapshot } from "../bridge/client";

// Whether an exchange needs explicit close-today / close-yesterday orders. The
// rule comes from the core; an exchange it does not list is treated as
// explicit, the conservative choice.
export function explicitCloseBuckets(snapshot: Snapshot | null, venue: string) {
  return (snapshot?.close_policies?.[venue] ?? "explicit_buckets") === "explicit_buckets";
}
