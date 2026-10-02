import type { Snapshot, TerminalCommand } from "../bridge/client";
import { NodeServices } from "./NodeServices";
export function Connections({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
}) {
  return (
    <section className="service-connections">
      <NodeServices snapshot={snapshot} busy={busy} trade={trade} />
    </section>
  );
}
