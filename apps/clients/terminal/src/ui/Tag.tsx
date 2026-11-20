import type { ReactNode } from "react";

// What something is, such as an account's environment; states use StatusBadge.
export function Tag({ danger, children }: { danger?: boolean; children: ReactNode }) {
  return (
    <span className="tag" data-tone={danger ? "danger" : undefined}>
      {children}
    </span>
  );
}
