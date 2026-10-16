import type { CommandHandler } from "../bridge/commands";
import type { Snapshot } from "../bridge/client";
import { NodeServices } from "./NodeServices";
export function Connections({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  return (
    <section className="service-connections">
      <NodeServices snapshot={snapshot} busy={busy} trade={trade} />
    </section>
  );
}
