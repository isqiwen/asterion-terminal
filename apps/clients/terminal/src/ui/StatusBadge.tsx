import type { ReactNode } from "react";

// Connection, authorization, task and run states share one badge.
export type Tone = "ok" | "attention" | "danger" | "neutral";

export function StatusBadge({ tone, children }: { tone: Tone; children: ReactNode }) {
  return (
    <span className="status-badge" data-tone={tone}>
      {children}
    </span>
  );
}
